#include "backend.h"

// Standard C++ includes
#include <algorithm>
#include <iterator>

// GeNN includes
#include "gennUtils.h"
#include "logging.h"

// GeNN code generator includes
#include "code_generator/codeStream.h"
#include "code_generator/codeGenUtils.h"
#include "code_generator/modelSpecMerged.h"
#include "code_generator/substitutions.h"

// OpenCL backend includes
#include "utils.h"

using namespace CodeGenerator;

//--------------------------------------------------------------------------
// Anonymous namespace
//--------------------------------------------------------------------------
namespace 
{
//! TO BE IMPLEMENTED - Use OpenCL functions - clRNG
const std::vector<Substitutions::FunctionTemplate> openclFunctions = {
    {"gennrand_uniform", 0, "uniform_double($(rng))", "uniform_clrngLfsr113($(rng))"},
    {"gennrand_normal", 0, "normal_double($(rng))", "normal($(rng))"},
    {"gennrand_exponential", 0, "exponentialDistDouble($(rng))", "exponentialDistFloat($(rng))"},
    {"gennrand_log_normal", 2, "log_normal_double($(rng), $(0), $(1))", "log_normal_float($(rng), $(0), $(1))"},
    {"gennrand_gamma", 1, "gammaDistDouble($(rng), $(0))", "gammaDistFloat($(rng), $(0))"}
};

//--------------------------------------------------------------------------
// Timer
//--------------------------------------------------------------------------
class Timer {
public:
    //! TO BE REVIEWED
    Timer(CodeStream& codeStream, const std::string& name, bool timingEnabled, bool synchroniseOnStop = false)
        : m_CodeStream(codeStream), m_Name(name), m_TimingEnabled(timingEnabled), m_SynchroniseOnStop(synchroniseOnStop)
    {

    }
private:
    //--------------------------------------------------------------------------
    // Members
    //--------------------------------------------------------------------------
    CodeStream& m_CodeStream;
    const std::string m_Name;
    const bool m_TimingEnabled;
    const bool m_SynchroniseOnStop;
};

//-----------------------------------------------------------------------
bool isSparseInitRequired(const SynapseGroupInternal& sg)
{
    return ((sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE)
        && (sg.isWUVarInitRequired() || !sg.getWUModel()->getLearnPostCode().empty() || !sg.getWUModel()->getSynapseDynamicsCode().empty()));
}
//--------------------------------------------------------------------------
void genAtomicAddFloat(CodeStream &os, const std::string &memoryType)
{
    os << "void atomic_add_f_" << memoryType << "(volatile __" << memoryType << " float *source, const float operand)";
    {
        CodeStream::Scope b(os);
        os << "union { unsigned int intVal; float floatVal; } newVal;" << std::endl;
        os << "union { unsigned int intVal; float floatVal; } prevVal;" << std::endl;
        os << "do";
        {
            CodeStream::Scope b(os);
            os << "prevVal.floatVal = *source;" << std::endl;
            os << "newVal.floatVal = prevVal.floatVal + operand;" << std::endl;
        }
        os << "while (atomic_cmpxchg((volatile __" << memoryType << " unsigned int *)source, prevVal.intVal, newVal.intVal) != prevVal.intVal);" << std::endl;
    }

    os << std::endl;
}
//--------------------------------------------------------------------------
template<typename T>
void genMergedGroupKernelParams(CodeStream &os, const std::vector<T> &groups, const std::string &name, bool includeFinalComma = false)
{
    // Loop through groups and add pointer
    // **NOTE** ideally we'd use __constant here (which in OpenCL appears to be more of a hint) but seems to cause weird ptx errors
    for(size_t i = 0; i < groups.size(); i++) {
        os << "__global struct Merged" << name << "Group" << i << " *d_merged" << name << "Group" << i;
        if(includeFinalComma || i != (groups.size() - 1)) {
            os << ", ";
        }
    }
}
//--------------------------------------------------------------------------
template<typename T>
void setMergedGroupKernelParams(CodeStream &os, const std::string &kernelName, const std::vector<T> &groups, const std::string &name)
{
    // Loop through groups and set as kernel arguments
    for(size_t i = 0; i < groups.size(); i++) {
        os << "CHECK_OPENCL_ERRORS(" << kernelName << ".setArg(" << i << ", d_merged" << name << "Group" << i << "));" << std::endl;
    }
}
//-----------------------------------------------------------------------
void genGroupStartIDs(CodeStream &, size_t &, size_t)
{
}
//-----------------------------------------------------------------------
template<typename T, typename G, typename ...Args>
void genGroupStartIDs(CodeStream &os, size_t &idStart, size_t workgroupSize,
                      const std::vector<T> &mergedGroups, const std::string &groupStartPrefix, G getNumThreads,
                      Args... args)
{
    // Loop through merged groups
    for(const auto &m : mergedGroups) {
        // Declare array of starting thread indices for each neuron group
        os << "__constant unsigned int d_merged" << groupStartPrefix << "GroupStartID" << m.getIndex() << "[] = {";
        for(const auto &ng : m.getGroups()) {
            os << idStart << ", ";
            idStart += padSize(getNumThreads(ng.get()), workgroupSize);
        }
        os << "};" << std::endl;
    }

    // Generate any remaining groups
    genGroupStartIDs(os, idStart, workgroupSize, args...);
}
//-----------------------------------------------------------------------
template<typename ...Args>
void genMergedKernelDataStructures(CodeStream &os, size_t workgroupSize, Args... args)
{
    // Generate group start id arrays
    size_t idStart = 0;
    genGroupStartIDs(os, std::ref(idStart), workgroupSize, args...);
}
}

//--------------------------------------------------------------------------
// CodeGenerator::OpenCL::Backend
//--------------------------------------------------------------------------
namespace CodeGenerator
{
namespace OpenCL
{
const char* Backend::KernelNames[KernelMax] = {
    "updateNeuronsKernel",
    "updatePresynapticKernel",
    "updatePostsynapticKernel",
    "updateSynapseDynamicsKernel",
    "initializeKernel",
    "initializeSparseKernel",
    "preNeuronResetKernel",
    "preSynapseResetKernel" };
//--------------------------------------------------------------------------
std::vector<PresynapticUpdateStrategy::Base*> Backend::s_PresynapticUpdateStrategies = {
    new PresynapticUpdateStrategy::PreSpan,
    new PresynapticUpdateStrategy::PostSpan,
};
//--------------------------------------------------------------------------
Backend::Backend(const KernelWorkGroupSize& kernelWorkGroupSizes, const Preferences& preferences,
                 const std::string& scalarType, unsigned int platformIndex, unsigned int deviceIndex)
:   BackendBase(scalarType), m_KernelWorkGroupSizes(kernelWorkGroupSizes), m_Preferences(preferences), 
    m_ChosenPlatformIndex(platformIndex), m_ChosenDeviceIndex(deviceIndex)
{
    assert(!m_Preferences.automaticCopy);

    // Get platforms
    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);
    assert(m_ChosenPlatformIndex < platforms.size());

    // Show platform name
    LOGI << "Using OpenCL platform:" << platforms[m_ChosenPlatformIndex].getInfo<CL_PLATFORM_NAME>();

    // Get platform devices
    std::vector<cl::Device> platformDevices;
    platforms[m_ChosenPlatformIndex].getDevices(CL_DEVICE_TYPE_ALL, &platformDevices);
    assert(m_ChosenDeviceIndex < platformDevices.size());

    // Select device
    m_ChosenDevice = platformDevices[m_ChosenDeviceIndex];
    
    // Show device name
    LOGI << "Using OpenCL device:" << m_ChosenDevice.getInfo<CL_DEVICE_NAME>();
}
//--------------------------------------------------------------------------
void Backend::genNeuronUpdate(CodeStream &os, const ModelSpecMerged &modelMerged, MemorySpaces&,
                              HostHandler preambleHandler, NeuronGroupSimHandler simHandler, NeuronUpdateGroupMergedHandler wuVarUpdateHandler,
                              HostHandler pushEGPHandler) const
{
    // Generate reset kernel to be run before the neuron kernel
    const ModelSpecInternal &model = modelMerged.getModel();

    os << "//--------------------------------------------------------------------------" << std::endl;
    os << "// OpenCL program and kernels" << std::endl;
    os << "//--------------------------------------------------------------------------" << std::endl;
    os << "cl::Program neuronUpdateProgram;" << std::endl;
    os << "cl::Kernel " << KernelNames[KernelPreNeuronReset] << ";" << std::endl;
    os << "cl::Kernel " << KernelNames[KernelNeuronUpdate] << ";" << std::endl;
    genMergedStructPreamble(os, modelMerged.getMergedNeuronSpikeQueueUpdateGroups(), "NeuronSpikeQueueUpdate");
    genMergedStructPreamble(os, modelMerged.getMergedNeuronUpdateGroups(), "NeuronUpdate");
    os << std::endl;

    // Generate preamble
    preambleHandler(os);

    //! KernelPreNeuronReset START
    size_t idPreNeuronReset = 0;

    // Creating the kernel body separately so it can be split into multiple string literals
    std::stringstream neuronUpdateKernelsStream;
    CodeStream neuronUpdateKernels(neuronUpdateKernelsStream);

    // Include definitions
    genKernelPreamble(neuronUpdateKernels, modelMerged);
    neuronUpdateKernels << std::endl << std::endl;

    // Generate support code
    modelMerged.genNeuronUpdateGroupSupportCode(neuronUpdateKernels);
    neuronUpdateKernels << std::endl << std::endl;
    
    // Generate struct definitions
    modelMerged.genMergedNeuronUpdateGroupStructs(neuronUpdateKernels, *this);
    modelMerged.genMergedNeuronSpikeQueueUpdateStructs(neuronUpdateKernels, *this);

    // Generate merged data structures
    genMergedKernelDataStructures(neuronUpdateKernels, m_KernelWorkGroupSizes[KernelNeuronUpdate],
                                  modelMerged.getMergedNeuronUpdateGroups(), "NeuronUpdate",
                                  [](const NeuronGroupInternal &ng) { return ng.getNumNeurons(); });
    neuronUpdateKernels << std::endl;

    // Generate kernels used to populate merged structs
    genMergedStructBuildKernels(neuronUpdateKernels, modelMerged.getMergedNeuronSpikeQueueUpdateGroups(), "NeuronSpikeQueueUpdate");
    genMergedStructBuildKernels(neuronUpdateKernels, modelMerged.getMergedNeuronUpdateGroups(), "NeuronUpdate");

    // Declare neuron spike queue update kernel
    neuronUpdateKernels << "__kernel void " << KernelNames[KernelPreNeuronReset] << "(";
    genMergedGroupKernelParams(neuronUpdateKernels, modelMerged.getMergedNeuronSpikeQueueUpdateGroups(), "NeuronSpikeQueueUpdate");
    neuronUpdateKernels << ")";
    {
        CodeStream::Scope b(neuronUpdateKernels);

        neuronUpdateKernels << "const unsigned int id = get_global_id(0);" << std::endl;

        // Loop through local neuron groups
        for(const auto &n : modelMerged.getMergedNeuronSpikeQueueUpdateGroups()) {
            neuronUpdateKernels << "// merged" << n.getIndex() << std::endl;
            if(idPreNeuronReset == 0) {
                neuronUpdateKernels << "if(id < " << n.getGroups().size() << ")";
            }
            else {
                neuronUpdateKernels << "if(id >= " << idPreNeuronReset << " && id < " << idPreNeuronReset + n.getGroups().size() << ")";
            }
            {
                CodeStream::Scope b(neuronUpdateKernels);

                // Use this to get reference to merged group structure
                neuronUpdateKernels << "__global struct MergedNeuronSpikeQueueUpdateGroup" << n.getIndex() << " *group = &d_mergedNeuronSpikeQueueUpdateGroup" << n.getIndex() << "[id - " << idPreNeuronReset << "]; " << std::endl;

                if(n.getArchetype().isDelayRequired()) { // with delay
                    neuronUpdateKernels << "*group->spkQuePtr  = (*group->spkQuePtr + 1) % " << n.getArchetype().getNumDelaySlots() << ";" << std::endl;
                }
                n.genMergedGroupSpikeCountReset(neuronUpdateKernels);
            }
            idPreNeuronReset += n.getGroups().size();
        }
    }
    neuronUpdateKernels << std::endl;
    //! KernelPreNeuronReset END
    size_t idStart = 0;

    //! KernelNeuronUpdate BODY START
    neuronUpdateKernels << "__kernel void " << KernelNames[KernelNeuronUpdate] << "(";
    genMergedGroupKernelParams(neuronUpdateKernels, modelMerged.getMergedNeuronUpdateGroups(), "NeuronUpdate", true);
    neuronUpdateKernels << model.getTimePrecision() << " t)";
    {
        CodeStream::Scope b(neuronUpdateKernels);
        neuronUpdateKernels << "const unsigned int localId = get_local_id(0);" << std::endl;
        neuronUpdateKernels << "const unsigned int id = get_global_id(0);" << std::endl;

        Substitutions kernelSubs(openclFunctions, model.getPrecision());
        kernelSubs.addVarSubstitution("t", "t");

        // If any neuron groups emit spike events
        if(std::any_of(modelMerged.getMergedNeuronUpdateGroups().cbegin(), modelMerged.getMergedNeuronUpdateGroups().cend(),
                       [](const NeuronUpdateGroupMerged &n) { return n.getArchetype().isSpikeEventRequired(); }))
        {
            neuronUpdateKernels << "volatile __local unsigned int shSpkEvnt[" << m_KernelWorkGroupSizes[KernelNeuronUpdate] << "];" << std::endl;
            neuronUpdateKernels << "volatile __local unsigned int shPosSpkEvnt;" << std::endl;
            neuronUpdateKernels << "volatile __local unsigned int shSpkEvntCount;" << std::endl;
            neuronUpdateKernels << std::endl;
            neuronUpdateKernels << "if (localId == 1)";
            {
                CodeStream::Scope b(neuronUpdateKernels);
                neuronUpdateKernels << "shSpkEvntCount = 0;" << std::endl;
            }
            neuronUpdateKernels << std::endl;
        }

        // If any neuron groups emit true spikes
        if(std::any_of(modelMerged.getMergedNeuronUpdateGroups().cbegin(), modelMerged.getMergedNeuronUpdateGroups().cend(),
                       [](const NeuronUpdateGroupMerged &n) { return !n.getArchetype().getNeuronModel()->getThresholdConditionCode().empty(); }))
        {
            neuronUpdateKernels << "volatile __local unsigned int shSpk[" << m_KernelWorkGroupSizes[KernelNeuronUpdate] << "];" << std::endl;
            neuronUpdateKernels << "volatile __local unsigned int shPosSpk;" << std::endl;
            neuronUpdateKernels << "volatile __local unsigned int shSpkCount;" << std::endl;
            neuronUpdateKernels << "if (localId == 0)";
            {
                CodeStream::Scope b(neuronUpdateKernels);
                neuronUpdateKernels << "shSpkCount = 0;" << std::endl;
            }
            neuronUpdateKernels << std::endl;
        }

        neuronUpdateKernels << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;

        // Parallelise over neuron groups
        genParallelGroup<NeuronUpdateGroupMerged>(neuronUpdateKernels, kernelSubs, modelMerged.getMergedNeuronUpdateGroups(), "NeuronUpdate", idStart,
            [this](const NeuronGroupInternal &ng) { return padSize(ng.getNumNeurons(), getKernelWorkGroupSize(KernelNeuronUpdate)); },
            [&model, simHandler, wuVarUpdateHandler, this](CodeStream &os, const NeuronUpdateGroupMerged &ng, Substitutions &popSubs)
            {
                // If axonal delays are required
                if(ng.getArchetype().isDelayRequired()) {
                    // We should READ from delay slot before spkQuePtr
                    os << "const unsigned int readDelayOffset = " << ng.getPrevQueueOffset() << ";" << std::endl;

                    // And we should WRITE to delay slot pointed to be spkQuePtr
                    os << "const unsigned int writeDelayOffset = " << ng.getCurrentQueueOffset() << ";" << std::endl;
                }
                os << std::endl;


                // If this neuron group requires a simulation RNG, substitute in this neuron group's RNG
                //! TO BE IMPLEMENTED - Not using rng at this point - 2020/03/08
                if (ng.getArchetype().isSimRNGRequired()) {
                    popSubs.addVarSubstitution("rng", "&group->rng[" + popSubs["id"] + "]");
                }

                // Call handler to generate generic neuron code
                os << "if(" << popSubs["id"] << " < group->numNeurons)";
                {
                    CodeStream::Scope b(os);
                    simHandler(os, ng, popSubs,
                        // Emit true spikes
                        [this](CodeStream& neuronUpdateKernelsBody, const NeuronUpdateGroupMerged &, Substitutions& subs)
                        {
                            genEmitSpike(neuronUpdateKernelsBody, subs, "");
                        },
                        // Emit spike-like events
                        [this](CodeStream& neuronUpdateKernelsBody, const NeuronUpdateGroupMerged &, Substitutions& subs)
                        {
                            genEmitSpike(neuronUpdateKernelsBody, subs, "Evnt");
                        });
                }

                os << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;

                if (ng.getArchetype().isSpikeEventRequired()) {
                    os << "if (localId == 1)";
                    {
                        CodeStream::Scope b(os);
                        os << "if (shSpkEvntCount > 0)";
                        {
                            CodeStream::Scope b(os);
                            os << "shPosSpkEvnt = atomic_add(&group->spkCntEvnt";
                            if (ng.getArchetype().isDelayRequired()) {
                                os << "[*group->spkQuePtr], shSpkEvntCount);" << std::endl;
                            }
                            else {
                                os << "[0], shSpkEvntCount);" << std::endl;
                            }
                        }
                    } // end if (localId == 0)
                    os << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;
                }

                if (!ng.getArchetype().getNeuronModel()->getThresholdConditionCode().empty()) {
                    os << "if (localId == 0)";
                    {
                        CodeStream::Scope b(os);
                        os << "if (shSpkCount > 0)";
                        {
                            CodeStream::Scope b(os);
                            os << "shPosSpk = atomic_add(&group->spkCnt";
                            if (ng.getArchetype().isDelayRequired() && ng.getArchetype().isTrueSpikeRequired()) {
                                os << "[*group->spkQuePtr], shSpkCount);" << std::endl;
                            }
                            else {
                                os << "[0], shSpkCount);" << std::endl;
                            }
                        }
                    } // end if (localId == 1)
                    os << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;
                }

                const std::string queueOffset = ng.getArchetype().isDelayRequired() ? "writeDelayOffset + " : "";
                if (ng.getArchetype().isSpikeEventRequired()) {
                    os << "if (localId < shSpkEvntCount)";
                    {
                        CodeStream::Scope b(os);
                        os << "group->spkEvnt[" << queueOffset << "shPosSpkEvnt + localId] = shSpkEvnt[localId];" << std::endl;
                    }
                }

                if (!ng.getArchetype().getNeuronModel()->getThresholdConditionCode().empty()) {
                    const std::string queueOffsetTrueSpk = ng.getArchetype().isTrueSpikeRequired() ? queueOffset : "";

                    os << "if (localId < shSpkCount)";
                    {
                        CodeStream::Scope b(os);

                        os << "const unsigned int n = shSpk[localId];" << std::endl;

                        // Create new substition stack and explicitly replace id with 'n' and perform WU var update
                        Substitutions wuSubs(&popSubs);
                        wuSubs.addVarSubstitution("id", "n", true);
                        wuVarUpdateHandler(os, ng, wuSubs);

                        os << "group->spk[" << queueOffsetTrueSpk << "shPosSpk + localId] = n;" << std::endl;
                        if (ng.getArchetype().isSpikeTimeRequired()) {
                            os << "group->spk[" << queueOffset << "n] = t;" << std::endl;
                        }
                    }
                }
            }
        );
    }
    neuronUpdateKernels << std::endl;

    // Write out kernel source string literal
    os << "const char* neuronUpdateSrc = ";
    divideKernelStreamInParts(os, neuronUpdateKernelsStream, 5000);
    os << ";" << std::endl;
    os << std::endl;

    // Function for initializing the KernelNeuronUpdate kernels
    os << "// Initialize the neuronUpdate kernels" << std::endl;
    os << "void buildNeuronUpdateProgram()";
    {
        CodeStream::Scope b(os);
        os << "// Build program" << std::endl;
        os << "neuronUpdateProgram = cl::Program(clContext, neuronUpdateSrc, true);" << std::endl;
        os << "if(neuronUpdateProgram.build(\"-cl-std=CL1.2 -I clRNG/include\") != CL_SUCCESS)";
        {
            CodeStream::Scope b(os);
            os << "throw std::runtime_error(\"Compile error:\" + neuronUpdateProgram.getBuildInfo<CL_PROGRAM_BUILD_LOG>(clDevice));" << std::endl;
        }
        os << std::endl;
        
        os << "// Configure merged struct buffers and kernels" << std::endl;
        genMergedStructBuild(os, modelMerged.getMergedNeuronSpikeQueueUpdateGroups(), "NeuronSpikeQueueUpdate", "neuronUpdateProgram");
        genMergedStructBuild(os, modelMerged.getMergedNeuronUpdateGroups(), "NeuronUpdate", "neuronUpdateProgram");
        os << std::endl;

        // KernelPreNeuronReset initialization
        if(idPreNeuronReset > 0) {
            os << "// Configure neuron spike queue update kernel" << std::endl;
            os << KernelNames[KernelPreNeuronReset] << " = cl::Kernel(neuronUpdateProgram, \"" << KernelNames[KernelPreNeuronReset] << "\");" << std::endl;
            setMergedGroupKernelParams(os, KernelNames[KernelPreNeuronReset], modelMerged.getMergedNeuronSpikeQueueUpdateGroups(), "NeuronSpikeQueueUpdate");
            os << std::endl;
        }

        // KernelNeuronUpdate initialization
        if(idStart > 0) {
            os << "// Configure neuron update kernel" << std::endl;
            os << KernelNames[KernelNeuronUpdate] << " = cl::Kernel(neuronUpdateProgram, \"" << KernelNames[KernelNeuronUpdate] << "\");" << std::endl;
            setMergedGroupKernelParams(os, KernelNames[KernelNeuronUpdate], modelMerged.getMergedNeuronUpdateGroups(), "NeuronUpdate");
            os << std::endl;
        }
    }

    os << std::endl;

    os << "void updateNeurons(" << model.getTimePrecision() << " t)";
    {
        CodeStream::Scope b(os);
        if (idPreNeuronReset > 0) {
            CodeStream::Scope b(os);
            genKernelDimensions(os, KernelPreNeuronReset, idPreNeuronReset);
            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueNDRangeKernel(" << KernelNames[KernelPreNeuronReset] << ", cl::NullRange, globalWorkSize, localWorkSize));" << std::endl;
            os << std::endl;
        }
        if (idStart > 0) {
            CodeStream::Scope b(os);
            os << "CHECK_OPENCL_ERRORS(" << KernelNames[KernelNeuronUpdate] << ".setArg(" << modelMerged.getMergedNeuronUpdateGroups().size() << ", t));" << std::endl;
            os << std::endl;
            genKernelDimensions(os, KernelNeuronUpdate, idStart);
            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueNDRangeKernel(" << KernelNames[KernelNeuronUpdate] << ", cl::NullRange, globalWorkSize, localWorkSize));" << std::endl;
        }
    }
}
//--------------------------------------------------------------------------
void Backend::genSynapseUpdate(CodeStream &os, const ModelSpecMerged &modelMerged, MemorySpaces&,
                               HostHandler preambleHandler, PresynapticUpdateGroupMergedHandler wumThreshHandler, PresynapticUpdateGroupMergedHandler wumSimHandler,
                               PresynapticUpdateGroupMergedHandler wumEventHandler, PresynapticUpdateGroupMergedHandler wumProceduralConnectHandler,
                               PostsynapticUpdateGroupMergedHandler postLearnHandler, SynapseDynamicsGroupMergedHandler synapseDynamicsHandler,
                               HostHandler pushEGPHandler) const
{
    // Generate reset kernel to be run before the neuron kernel
    const ModelSpecInternal &model = modelMerged.getModel();

    os << "//--------------------------------------------------------------------------" << std::endl;
    os << "// OpenCL program and kernels" << std::endl;
    os << "//--------------------------------------------------------------------------" << std::endl;
    os << "cl::Program synapseUpdateProgram;" << std::endl;
    os << "cl::Kernel " << KernelNames[KernelPreSynapseReset] << ";" << std::endl;
    os << "cl::Kernel " << KernelNames[KernelPresynapticUpdate] << ";" << std::endl;
    os << "cl::Kernel " << KernelNames[KernelPostsynapticUpdate] << ";" << std::endl;
    os << "cl::Kernel " << KernelNames[KernelSynapseDynamicsUpdate] << ";" << std::endl;
    genMergedStructPreamble(os, modelMerged.getMergedSynapseDendriticDelayUpdateGroups(), "SynapseDendriticDelayUpdate");
    genMergedStructPreamble(os, modelMerged.getMergedPresynapticUpdateGroups(), "PresynapticUpdate");
    genMergedStructPreamble(os, modelMerged.getMergedPostsynapticUpdateGroups(), "PostsynapticUpdate");
    genMergedStructPreamble(os, modelMerged.getMergedSynapseDynamicsGroups(), "SynapseDynamics");

    // Generate preamble
    preambleHandler(os);
    
    // Creating the kernel body separately so it can be split into multiple string literals
    std::stringstream synapseUpdateKernelsStream;
    CodeStream synapseUpdateKernels(synapseUpdateKernelsStream);
    
    // Include definitions
    genKernelPreamble(synapseUpdateKernels, modelMerged);
    synapseUpdateKernels << "// ------------------------------------------------------------------------" << std::endl;
    synapseUpdateKernels << "// bit tool macros" << std::endl;
    synapseUpdateKernels << "#define B(x,i) ((x) & (0x80000000 >> (i))) //!< Extract the bit at the specified position i from x" << std::endl;
    synapseUpdateKernels << "#define setB(x,i) x= ((x) | (0x80000000 >> (i))) //!< Set the bit at the specified position i in x to 1" << std::endl;
    synapseUpdateKernels << "#define delB(x,i) x= ((x) & (~(0x80000000 >> (i)))) //!< Set the bit at the specified position i in x to 0" << std::endl;
    synapseUpdateKernels << std::endl << std::endl;
  
    genAtomicAddFloat(synapseUpdateKernels, "local");
    genAtomicAddFloat(synapseUpdateKernels, "global");

    // Generate support code
    modelMerged.genPresynapticUpdateSupportCode(synapseUpdateKernels);
    modelMerged.genPostsynapticUpdateSupportCode(synapseUpdateKernels);
    modelMerged.genSynapseDynamicsSupportCode(synapseUpdateKernels);
    synapseUpdateKernels << std::endl;

    // Generate struct definitions
    modelMerged.genMergedSynapseDendriticDelayUpdateStructs(synapseUpdateKernels, *this);
    modelMerged.genMergedPresynapticUpdateGroupStructs(synapseUpdateKernels, *this);
    modelMerged.genMergedPostsynapticUpdateGroupStructs(synapseUpdateKernels, *this);
    modelMerged.genMergedSynapseDynamicsGroupStructs(synapseUpdateKernels, *this);
    synapseUpdateKernels << std::endl;

    // Generate data structure for accessing merged groups
    genMergedKernelDataStructures(synapseUpdateKernels, m_KernelWorkGroupSizes[KernelPresynapticUpdate],
                                  modelMerged.getMergedPresynapticUpdateGroups(), "PresynapticUpdate",
                                  [this](const SynapseGroupInternal &sg){ return getNumPresynapticUpdateThreads(sg); });
    genMergedKernelDataStructures(synapseUpdateKernels, m_KernelWorkGroupSizes[KernelPostsynapticUpdate],
                                  modelMerged.getMergedPostsynapticUpdateGroups(), "PostsynapticUpdate",
                                  [this](const SynapseGroupInternal &sg) { return getNumPostsynapticUpdateThreads(sg); });
    genMergedKernelDataStructures(synapseUpdateKernels, m_KernelWorkGroupSizes[KernelSynapseDynamicsUpdate],
                                  modelMerged.getMergedSynapseDynamicsGroups(), "SynapseDynamics",
                                  [this](const SynapseGroupInternal &sg) { return getNumSynapseDynamicsThreads(sg); });

    // Generate kernels used to populate merged structs
    genMergedStructBuildKernels(synapseUpdateKernels, modelMerged.getMergedSynapseDendriticDelayUpdateGroups(), "SynapseDendriticDelayUpdate");
    genMergedStructBuildKernels(synapseUpdateKernels, modelMerged.getMergedPresynapticUpdateGroups(), "PresynapticUpdate");
    genMergedStructBuildKernels(synapseUpdateKernels, modelMerged.getMergedPostsynapticUpdateGroups(), "PostsynapticUpdate");
    genMergedStructBuildKernels(synapseUpdateKernels, modelMerged.getMergedSynapseDynamicsGroups(), "SynapseDynamics");

    // Declare neuron spike queue update kernel
    size_t idPreSynapseReset = 0;
    if(!modelMerged.getMergedSynapseDendriticDelayUpdateGroups().empty()) {
        synapseUpdateKernels << "__kernel void " << KernelNames[KernelPreSynapseReset] << "(";
        genMergedGroupKernelParams(synapseUpdateKernels, modelMerged.getMergedSynapseDendriticDelayUpdateGroups(), "SynapseDendriticDelayUpdate");
        synapseUpdateKernels << ")";
        {
            CodeStream::Scope b(synapseUpdateKernels);

            synapseUpdateKernels << "const unsigned int id = get_global_id(0);" << std::endl;

            // Loop through merged synapse groups
            for(const auto &n : modelMerged.getMergedSynapseDendriticDelayUpdateGroups()) {
                synapseUpdateKernels << "// merged" << n.getIndex() << std::endl;
                if(idPreSynapseReset == 0) {
                    synapseUpdateKernels << "if(id < " << n.getGroups().size() << ")";
                }
                else {
                    synapseUpdateKernels << "if(id >= " << idPreSynapseReset << " && id < " << idPreSynapseReset + n.getGroups().size() << ")";
                }
                {
                    CodeStream::Scope b(os);

                    // Use this to get reference to merged group structure
                    synapseUpdateKernels << "MergedSynapseDendriticDelayUpdateGroup" << n.getIndex() << " *group = &d_mergedSynapseDendriticDelayUpdateGroup" << n.getIndex() << "[id - " << idPreSynapseReset << "]; " << std::endl;

                    synapseUpdateKernels << "*group->denDelayPtr = (*group->denDelayPtr + 1) % " << n.getArchetype().getMaxDendriticDelayTimesteps() << ";" << std::endl;
                }
                idPreSynapseReset += n.getGroups().size();
            }
            os << std::endl;
        }
    }

    // If there are any presynaptic update groups
    size_t idPresynapticStart = 0;
    if(!modelMerged.getMergedPresynapticUpdateGroups().empty()) {
        synapseUpdateKernels << "__kernel void " << KernelNames[KernelPresynapticUpdate] << "(";
        genMergedGroupKernelParams(synapseUpdateKernels, modelMerged.getMergedPresynapticUpdateGroups(), "PresynapticUpdate", true);
        synapseUpdateKernels << model.getTimePrecision() << " t)";
        {
            CodeStream::Scope b(synapseUpdateKernels);

            Substitutions kernelSubs(openclFunctions, model.getPrecision());
            kernelSubs.addVarSubstitution("t", "t");

            synapseUpdateKernels << "const unsigned int localId = get_local_id(0);" << std::endl;
            synapseUpdateKernels << "const unsigned int id = get_global_id(0);" << std::endl;

            // We need shLg if any synapse groups accumulate into shared memory
            if(std::any_of(modelMerged.getMergedPresynapticUpdateGroups().cbegin(), modelMerged.getMergedPresynapticUpdateGroups().cend(),
                           [this](const PresynapticUpdateGroupMerged &sg)
            {
                return getPresynapticUpdateStrategy(sg.getArchetype())->shouldAccumulateInSharedMemory(sg, *this);
            }))
            {
                synapseUpdateKernels << "__local " << model.getPrecision() << " shLg[" << m_KernelWorkGroupSizes[KernelPresynapticUpdate] << "];" << std::endl;
            }

            // If any of these synapse groups also have sparse connectivity, allocate shared memory for row length
            if(std::any_of(modelMerged.getMergedPresynapticUpdateGroups().cbegin(), modelMerged.getMergedPresynapticUpdateGroups().cend(),
                           [](const PresynapticUpdateGroupMerged &sg)
                           {
                               return (sg.getArchetype().getSpanType() == SynapseGroup::SpanType::POSTSYNAPTIC
                                       && (sg.getArchetype().getMatrixType() & SynapseMatrixConnectivity::SPARSE));
                           }))
            {
                synapseUpdateKernels << "__local unsigned int shRowLength[" << m_KernelWorkGroupSizes[KernelPresynapticUpdate] << "];" << std::endl;
            }

            if(std::any_of(modelMerged.getMergedPresynapticUpdateGroups().cbegin(), modelMerged.getMergedPresynapticUpdateGroups().cend(),
                           [](const PresynapticUpdateGroupMerged &sg)
                           {
                               return (sg.getArchetype().isTrueSpikeRequired() || !sg.getArchetype().getWUModel()->getLearnPostCode().empty());
                           }))
            {
                synapseUpdateKernels << "__local unsigned int shSpk[" << m_KernelWorkGroupSizes[KernelPresynapticUpdate] << "];" << std::endl;
            }

            if(std::any_of(modelMerged.getMergedPresynapticUpdateGroups().cbegin(), modelMerged.getMergedPresynapticUpdateGroups().cend(),
                           [](const PresynapticUpdateGroupMerged &sg) { return (sg.getArchetype().isSpikeEventRequired()); }))
                           {
                               synapseUpdateKernels << "__local unsigned int shSpkEvnt[" << m_KernelWorkGroupSizes[KernelPresynapticUpdate] << "];" << std::endl;
                           }

            // Parallelise over synapse groups
            genParallelGroup<PresynapticUpdateGroupMerged>(synapseUpdateKernels, kernelSubs, modelMerged.getMergedPresynapticUpdateGroups(), "PresynapticUpdate", idPresynapticStart,
                [this](const SynapseGroupInternal &sg) { return padSize(getNumPresynapticUpdateThreads(sg), m_KernelWorkGroupSizes[KernelPresynapticUpdate]); },
                [wumThreshHandler, wumSimHandler, wumEventHandler, &modelMerged, this](CodeStream &os, const PresynapticUpdateGroupMerged &sg, const Substitutions &popSubs)
                {
                    // Get presynaptic update strategy to use for this synapse group
                    const auto *presynapticUpdateStrategy = getPresynapticUpdateStrategy(sg.getArchetype());
                    LOGD_BACKEND << "Using '" << typeid(*presynapticUpdateStrategy).name() << "' presynaptic update strategy for merged synapse group '" << sg.getIndex() << "'";

                    // If presynaptic neuron group has variable queues, calculate offset to read from its variables with axonal delay
                    if(sg.getArchetype().getSrcNeuronGroup()->isDelayRequired()) {
                        os << "const unsigned int preReadDelaySlot = " << sg.getPresynapticAxonalDelaySlot() << ";" << std::endl;
                        os << "const unsigned int preReadDelayOffset = preReadDelaySlot * group->numSrcNeurons;" << std::endl;
                    }

                    // If postsynaptic neuron group has variable queues, calculate offset to read from its variables at current time
                    if(sg.getArchetype().getTrgNeuronGroup()->isDelayRequired()) {
                        os << "const unsigned int postReadDelayOffset = " << sg.getPostsynapticBackPropDelaySlot() << " * group->numTrgNeurons;" << std::endl;
                    }

                    // If we are going to accumulate postsynaptic input into a register, zero register value
                    if(presynapticUpdateStrategy->shouldAccumulateInRegister(sg, *this)) {
                        os << "// only do this for existing neurons" << std::endl;
                        os << modelMerged.getModel().getPrecision() << " linSyn = 0;" << std::endl;
                    }
                    // Otherwise, if we are going to accumulate into shared memory, zero entry in array for each target neuron
                    // **NOTE** is ok as number of target neurons <= synapseBlkSz
                    else if(presynapticUpdateStrategy->shouldAccumulateInSharedMemory(sg, *this)) {
                        os << "if(localId < group->numTrgNeurons)";
                        {
                            CodeStream::Scope b(os);
                            os << "shLg[localId] = 0;" << std::endl;
                        }
                        os << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;
                    }

                    // If spike events should be processed
                    if(sg.getArchetype().isSpikeEventRequired()) {
                        CodeStream::Scope b(os);
                        presynapticUpdateStrategy->genCode(os, modelMerged, sg, popSubs, *this, false,
                                                           wumThreshHandler, wumEventHandler);
                    }

                    // If true spikes should be processed
                    if(sg.getArchetype().isTrueSpikeRequired()) {
                        CodeStream::Scope b(os);
                        presynapticUpdateStrategy->genCode(os, modelMerged, sg, popSubs, *this, true,
                                                           wumThreshHandler, wumSimHandler);
                    }
                    os << std::endl;

                    // If we have been accumulating into a register, write value back to global memory
                    if(presynapticUpdateStrategy->shouldAccumulateInRegister(sg, *this)) {
                        os << "// only do this for existing neurons" << std::endl;
                        os << "if (" << popSubs["id"] << " < group->numTrgNeurons)";
                        {
                            CodeStream::Scope b(os);
                            const std::string inSyn = "group->inSyn[" + popSubs["id"] + "]";
                            if(sg.getArchetype().isPSModelMerged()) {
                                os << getFloatAtomicAdd(modelMerged.getModel().getPrecision()) << "(&" << inSyn << ", linSyn);" << std::endl;
                            }
                            else {
                                os << inSyn << " += linSyn;" << std::endl;
                            }
                        }
                    }
                    // Otherwise, if we have been accumulating into shared memory, write value back to global memory
                    // **NOTE** is ok as number of target neurons <= synapseBlkSz
                    else if(presynapticUpdateStrategy->shouldAccumulateInSharedMemory(sg, *this)) {
                        os << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;
                        os << "if (localId < group->numTrgNeurons)";
                        {
                            CodeStream::Scope b(os);
                            const std::string inSyn = "group->inSyn[localId]";

                            if(sg.getArchetype().isPSModelMerged()) {
                                os << getFloatAtomicAdd(modelMerged.getModel().getPrecision()) << "(&" << inSyn << ", shLg[localId]);" << std::endl;
                            }
                            else {
                                os << inSyn << " += shLg[localId];" << std::endl;
                            }
                        }
                    }
                });
        }
    }

    // If any synapse groups require postsynaptic learning
    size_t idPostsynapticStart = 0;
    if(!modelMerged.getMergedPostsynapticUpdateGroups().empty()) {
        synapseUpdateKernels << "__kernel void " << KernelNames[KernelPostsynapticUpdate] << "(";
        genMergedGroupKernelParams(synapseUpdateKernels, modelMerged.getMergedPostsynapticUpdateGroups(), "PostsynapticUpdate", true);
        synapseUpdateKernels << model.getTimePrecision() << " t)";
        {
            CodeStream::Scope b(synapseUpdateKernels);
            Substitutions kernelSubs(openclFunctions, model.getPrecision());
            kernelSubs.addVarSubstitution("t", "t");

            synapseUpdateKernels << "const unsigned int localId = get_local_id(0);" << std::endl;
            synapseUpdateKernels << "const unsigned int id = get_global_id(0);" << std::endl;
            synapseUpdateKernels << "__local unsigned int shSpk[" << m_KernelWorkGroupSizes[KernelPostsynapticUpdate] << "];" << std::endl;
            if(std::any_of(modelMerged.getMergedPostsynapticUpdateGroups().cbegin(), modelMerged.getMergedPostsynapticUpdateGroups().cend(),
                           [](const PostsynapticUpdateGroupMerged &s)
                           {
                               return ((s.getArchetype().getMatrixType() & SynapseMatrixConnectivity::SPARSE) && !s.getArchetype().getWUModel()->getLearnPostCode().empty());
                           }))
            {
                synapseUpdateKernels << "__local unsigned int shColLength[" << m_KernelWorkGroupSizes[KernelPostsynapticUpdate] << "];" << std::endl;
            }

            // Parallelise over synapse groups whose weight update models have code for postsynaptic learning
            genParallelGroup<PostsynapticUpdateGroupMerged>(synapseUpdateKernels, kernelSubs, modelMerged.getMergedPostsynapticUpdateGroups(), "PostsynapticUpdate", idPostsynapticStart,
                [this](const SynapseGroupInternal &sg) { return padSize(getNumPostsynapticUpdateThreads(sg), m_KernelWorkGroupSizes[KernelPostsynapticUpdate]); },
                [postLearnHandler, this](CodeStream &os, const PostsynapticUpdateGroupMerged &sg, const Substitutions &popSubs)
                {
                    // If presynaptic neuron group has variable queues, calculate offset to read from its variables with axonal delay
                    if(sg.getArchetype().getSrcNeuronGroup()->isDelayRequired()) {
                        os << "const unsigned int preReadDelayOffset = " << sg.getPresynapticAxonalDelaySlot() << " * group->srcNumNeurons;" << std::endl;
                    }

                    // If postsynaptic neuron group has variable queues, calculate offset to read from its variables at current time
                    if(sg.getArchetype().getTrgNeuronGroup()->isDelayRequired()) {
                        os << "const unsigned int postReadDelaySlot = " << sg.getPostsynapticBackPropDelaySlot() << ";" << std::endl;
                        os << "const unsigned int postReadDelayOffset = postReadDelaySlot * group->trgNumNeurons;" << std::endl;
                    }

                    if(sg.getArchetype().getTrgNeuronGroup()->isDelayRequired() && sg.getArchetype().getTrgNeuronGroup()->isTrueSpikeRequired()) {
                        os << "const unsigned int numSpikes = group->trgSpkCnt[postReadDelaySlot];" << std::endl;
                    }
                    else {
                        os << "const unsigned int numSpikes = group->trgSpkCnt[0];" << std::endl;
                    }
                    
                    os << "const unsigned int numSpikeBlocks = (numSpikes + " << m_KernelWorkGroupSizes[KernelPostsynapticUpdate] - 1 << ") / " << m_KernelWorkGroupSizes[KernelPostsynapticUpdate] << ";" << std::endl;
                    os << "for (unsigned int r = 0; r < numSpikeBlocks; r++)";
                    {
                        CodeStream::Scope b(os);
                        os << "const unsigned int numSpikesInBlock = (r == numSpikeBlocks - 1) ? ((numSpikes - 1) % " << m_KernelWorkGroupSizes[KernelPostsynapticUpdate] << ") + 1 : " << m_KernelWorkGroupSizes[KernelPostsynapticUpdate] << ";" << std::endl;

                        os << "if (localId < numSpikesInBlock)";
                        {
                            CodeStream::Scope b(os);
                            const std::string offsetTrueSpkPost = (sg.getArchetype().getTrgNeuronGroup()->isTrueSpikeRequired() && sg.getArchetype().getTrgNeuronGroup()->isDelayRequired()) ? "postReadDelayOffset + " : "";
                            os << "const unsigned int spk = group->trgSpk[" << offsetTrueSpkPost << "(r * " << m_KernelWorkGroupSizes[KernelPostsynapticUpdate] << ") + localId];" << std::endl;
                            os << "shSpk[localId] = spk;" << std::endl;

                            if(sg.getArchetype().getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
                                os << "shColLength[localId] = group->colLength[spk];" << std::endl;
                            }
                        }

                        os << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;
                        os << "// only work on existing neurons" << std::endl;
                        os << "if (" << popSubs["id"] << " < group->colStride)";
                        {
                            CodeStream::Scope b(os);
                            os << "// loop through all incoming spikes for learning" << std::endl;
                            os << "for (unsigned int j = 0; j < numSpikesInBlock; j++)";
                            {
                                CodeStream::Scope b(os);

                                Substitutions synSubs(&popSubs);
                                if(sg.getArchetype().getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
                                    os << "if (" << popSubs["id"] << " < shColLength[j])" << CodeStream::OB(1540);
                                    os << "const unsigned int synAddress = group->remap[(shSpk[j] * group->colStride) + " << popSubs["id"] << "];" << std::endl;

                                    os << "const unsigned int ipre = synAddress / group->rowStride;" << std::endl;
                                    synSubs.addVarSubstitution("id_pre", "ipre");
                                }
                                else {
                                    os << "const unsigned int synAddress = (" << popSubs["id"] << " * group->trgNumNeurons) + shSpk[j];" << std::endl;
                                    synSubs.addVarSubstitution("id_pre", synSubs["id"]);
                                }

                                synSubs.addVarSubstitution("id_post", "shSpk[j]");
                                synSubs.addVarSubstitution("id_syn", "synAddress");

                                postLearnHandler(os, sg, synSubs);

                                if(sg.getArchetype().getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
                                    os << CodeStream::CB(1540);
                                }
                            }
                        }
                    }
                });
        }
    }
    //! KernelPostsynapticUpdate BODY END

    /*size_t idSynapseDynamicsStart = 0;
    std::stringstream synapseDynamicsUpdateKernelBodyStream;

    bool hasSynapseDynamicsUpdateKernel = std::any_of(model.getLocalSynapseGroups().cbegin(), model.getLocalSynapseGroups().cend(),
        [](const ModelSpec::SynapseGroupValueType& s) { return !s.second.getWUModel()->getSynapseDynamicsCode().empty(); });

    //! KernelSynapseDynamicsUpdate BODY START
    if (hasSynapseDynamicsUpdateKernel)
    {
        CodeStream synapseDynamicsUpdateKernelBody(synapseDynamicsUpdateKernelBodyStream);

        synapseDynamicsUpdateKernelBody << "const unsigned int localId = get_local_id(0);" << std::endl;
        synapseDynamicsUpdateKernelBody << "const unsigned int id = get_global_id(0);" << std::endl;

        Substitutions kernelSubs(openclFunctions, model.getPrecision());
        kernelSubs.addVarSubstitution("t", "t");

        // Parallelise over synapse groups whose weight update models have code for synapse dynamics
        genParallelGroup<SynapseGroupInternal>(synapseDynamicsUpdateKernelBody, kernelSubs, model.getLocalSynapseGroups(), idSynapseDynamicsStart, synapseDynamicsUpdateKernelParams,
            [this](const SynapseGroupInternal& sg) { return Utils::padSize(getNumSynapseDynamicsThreads(sg), m_KernelWorkGroupSizes[KernelSynapseDynamicsUpdate]); },
            [](const SynapseGroupInternal& sg) { return !sg.getWUModel()->getSynapseDynamicsCode().empty(); },
            [synapseDynamicsHandler, &model, this, &synapseDynamicsUpdateKernelParams](CodeStream& synapseDynamicsUpdateKernelBody, const SynapseGroupInternal& sg, const Substitutions& popSubs)
            {
                // If presynaptic neuron group has variable queues, calculate offset to read from its variables with axonal delay
                if (sg.getSrcNeuronGroup()->isDelayRequired()) {
                    synapseDynamicsUpdateKernelBody << "const unsigned int preReadDelayOffset = " << sg.getPresynapticAxonalDelaySlot("") << " * " << sg.getSrcNeuronGroup()->getNumNeurons() << ";" << std::endl;
                    synapseDynamicsUpdateKernelParams.insert({ "spkQuePtr" + sg.getSrcNeuronGroup()->getName(), "volatile unsigned int" });
                }

                // If postsynaptic neuron group has variable queues, calculate offset to read from its variables at current time
                if (sg.getTrgNeuronGroup()->isDelayRequired()) {
                    synapseDynamicsUpdateKernelBody << "const unsigned int postReadDelayOffset = " << sg.getPostsynapticBackPropDelaySlot("") << " * " << sg.getTrgNeuronGroup()->getNumNeurons() << ";" << std::endl;
                    synapseDynamicsUpdateKernelParams.insert({ "spkQuePtr" + sg.getTrgNeuronGroup()->getName(), "volatile unsigned int" });
                }

                Substitutions synSubs(&popSubs);

                if (sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
                    synapseDynamicsUpdateKernelBody << "if (" << popSubs["id"] << " < d_synRemap" << sg.getName() << "[0])";
                    synapseDynamicsUpdateKernelParams.insert({ "d_synRemap" + sg.getName(), "__global unsigned int*" });
                }
                else {
                    synapseDynamicsUpdateKernelBody << "if (" << popSubs["id"] << " < " << sg.getSrcNeuronGroup()->getNumNeurons() * sg.getTrgNeuronGroup()->getNumNeurons() << ")";
                }

                {
                    CodeStream::Scope b(synapseDynamicsUpdateKernelBody);

                    if (sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
                        // Determine synapse and presynaptic indices for this thread
                        synapseDynamicsUpdateKernelBody << "const unsigned int s = d_synRemap" << sg.getName() << "[1 + " << popSubs["id"] << "];" << std::endl;
                        // Parameter d_synRemap for kernel already inserted

                        synSubs.addVarSubstitution("id_pre", "s / " + std::to_string(sg.getMaxConnections()));
                        synSubs.addVarSubstitution("id_post", "d_ind" + sg.getName() + "[s]");
                        synSubs.addVarSubstitution("id_syn", "s");

                        synapseDynamicsUpdateKernelParams.insert({ "d_ind" + sg.getName(), "__global unsigned int*" });
                    }
                    else {
                        synSubs.addVarSubstitution("id_pre", popSubs["id"] + " / " + std::to_string(sg.getTrgNeuronGroup()->getNumNeurons()));
                        synSubs.addVarSubstitution("id_post", popSubs["id"] + " % " + std::to_string(sg.getTrgNeuronGroup()->getNumNeurons()));
                        synSubs.addVarSubstitution("id_syn", popSubs["id"]);
                    }

                    // If dendritic delay is required, always use atomic operation to update dendritic delay buffer
                    if (sg.isDendriticDelayRequired()) {
                        synSubs.addFuncSubstitution("addToInSynDelay", 2, getFloatAtomicAdd(model.getPrecision()) + "(&d_denDelay" + sg.getPSModelTargetName() + "[" + sg.getDendriticDelayOffset("", "$(1)") + synSubs["id_post"] + "], $(0))");

                        synapseDynamicsUpdateKernelParams.insert({ "d_denDelay" + sg.getPSModelTargetName(), "__global unsigned int*" });
                        synapseDynamicsUpdateKernelParams.insert({ "denDelayPtr" + sg.getPSModelTargetName(), "volatile unsigned int" });
                    }
                    // Otherwise
                    else {
                        synSubs.addFuncSubstitution("addToInSyn", 1, getFloatAtomicAdd(model.getPrecision()) + "(&d_inSyn" + sg.getPSModelTargetName() + "[" + synSubs["id_post"] + "], $(0))");
                        synapseDynamicsUpdateKernelParams.insert({ "d_inSyn" + sg.getPSModelTargetName(), "__global float*" });
                    }

                    synapseDynamicsHandler(synapseDynamicsUpdateKernelBody, sg, synSubs);
                }
            });
    }*/
    synapseUpdateKernels << std::endl;
    
    // Write out kernel source string literal
    os << "const char* synapseUpdateSrc = ";
    divideKernelStreamInParts(os, synapseUpdateKernelsStream, 5000);
    os << ";" << std::endl;
    os << std::endl;
    
    os << "// Initialize the synapseUpdate kernels" << std::endl;
    os << "void buildSynapseUpdateProgram()";
    {
        CodeStream::Scope b(os);
        os << "// Build program" << std::endl;
        os << "synapseUpdateProgram = cl::Program(clContext, synapseUpdateSrc, true);" << std::endl;
        os << "if(synapseUpdateProgram.build(\"-cl-std=CL1.2 -I clRNG/include\") != CL_SUCCESS)";
        {
            CodeStream::Scope b(os);
            os << "throw std::runtime_error(\"Compile error:\" + synapseUpdateProgram.getBuildInfo<CL_PROGRAM_BUILD_LOG>(clDevice));" << std::endl;
        }
        os << std::endl;

        os << "// Configure merged struct buffers and kernels" << std::endl;
        genMergedStructBuild(os, modelMerged.getMergedSynapseDendriticDelayUpdateGroups(), "SynapseDendriticDelayUpdate", "synapseUpdateProgram");
        genMergedStructBuild(os, modelMerged.getMergedPresynapticUpdateGroups(), "PresynapticUpdate", "synapseUpdateProgram");
        genMergedStructBuild(os, modelMerged.getMergedPostsynapticUpdateGroups(), "PostsynapticUpdate", "synapseUpdateProgram");
        os << std::endl;

        if(idPreSynapseReset > 0) {
            os << "// Configure dendritic delay update kernel" << std::endl;
            os << KernelNames[KernelPreSynapseReset] << " = cl::Kernel(synapseUpdateProgram, \"" << KernelNames[KernelPreSynapseReset] << "\");" << std::endl;
            setMergedGroupKernelParams(os, KernelNames[KernelPreSynapseReset], modelMerged.getMergedSynapseDendriticDelayUpdateGroups(), "SynapseDendriticDelayUpdate");
            os << std::endl;
        }

        if(idPresynapticStart > 0) {
            os << "// Configure presynaptic update kernel" << std::endl;
            os << KernelNames[KernelPresynapticUpdate] << " = cl::Kernel(synapseUpdateProgram, \"" << KernelNames[KernelPresynapticUpdate] << "\");" << std::endl;
            setMergedGroupKernelParams(os, KernelNames[KernelPresynapticUpdate], modelMerged.getMergedPresynapticUpdateGroups(), "PresynapticUpdate");
            os << std::endl;
        }

        if(idPostsynapticStart > 0) {
            os << "// Configure postsynaptic update kernel" << std::endl;
            os << KernelNames[KernelPostsynapticUpdate] << " = cl::Kernel(synapseUpdateProgram, \"" << KernelNames[KernelPostsynapticUpdate] << "\");" << std::endl;
            setMergedGroupKernelParams(os, KernelNames[KernelPostsynapticUpdate], modelMerged.getMergedPostsynapticUpdateGroups(), "PostsynapticUpdate");
            os << std::endl;
        }
    }

    os << std::endl;

    os << "void updateSynapses(" << modelMerged.getModel().getTimePrecision() << " t)";
    {
        CodeStream::Scope b(os);

        // Launch pre-synapse reset kernel if required
        if (idPreSynapseReset > 0) {
            CodeStream::Scope b(os);
            genKernelDimensions(os, KernelPreSynapseReset, idPreSynapseReset);
            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueNDRangeKernel(" << KernelNames[KernelPreSynapseReset] << ", cl::NullRange, globalWorkSize, localWorkSize));" << std::endl;
        }

        // Launch synapse dynamics kernel if required
        /*if (idSynapseDynamicsStart > 0) {
            CodeStream::Scope b(os);
            genKernelHostArgs(os, KernelSynapseDynamicsUpdate, synapseDynamicsUpdateKernelParams);
            os << "CHECK_OPENCL_ERRORS(" << KernelNames[KernelSynapseDynamicsUpdate] << ".setArg(" << synapseDynamicsUpdateKernelParams.size() << ", t));" << std::endl;
            os << std::endl;
            genKernelDimensions(os, KernelSynapseDynamicsUpdate, idSynapseDynamicsStart);
            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueNDRangeKernel(" << KernelNames[KernelSynapseDynamicsUpdate] << ", cl::NullRange, globalWorkSize, localWorkSize));" << std::endl;
            os << "CHECK_OPENCL_ERRORS(commandQueue.finish());" << std::endl;
        }*/

        // Launch presynaptic update kernel
        if (idPresynapticStart > 0) {
            CodeStream::Scope b(os);
            os << "CHECK_OPENCL_ERRORS(" << KernelNames[KernelPresynapticUpdate] << ".setArg(" << modelMerged.getMergedPresynapticUpdateGroups().size() << ", t));" << std::endl;
            os << std::endl;
            genKernelDimensions(os, KernelPresynapticUpdate, idPresynapticStart);
            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueNDRangeKernel(" << KernelNames[KernelPresynapticUpdate] << ", cl::NullRange, globalWorkSize, localWorkSize));" << std::endl;
        }

        // Launch postsynaptic update kernel
        if (idPostsynapticStart > 0) {
            CodeStream::Scope b(os);
            os << "CHECK_OPENCL_ERRORS(" << KernelNames[KernelPostsynapticUpdate] << ".setArg(" << modelMerged.getMergedPostsynapticUpdateGroups().size() << ", t));" << std::endl;
            os << std::endl;
            genKernelDimensions(os, KernelPostsynapticUpdate, idPostsynapticStart);
            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueNDRangeKernel(" << KernelNames[KernelPostsynapticUpdate] << ", cl::NullRange, globalWorkSize, localWorkSize));" << std::endl;
        }
    }
}
//--------------------------------------------------------------------------
void Backend::genInit(CodeStream &os, const ModelSpecMerged &modelMerged, MemorySpaces&,
                      HostHandler preambleHandler, NeuronInitGroupMergedHandler localNGHandler, SynapseDenseInitGroupMergedHandler sgDenseInitHandler,
                      SynapseConnectivityInitMergedGroupHandler sgSparseConnectHandler, SynapseSparseInitGroupMergedHandler sgSparseInitHandler,
                      HostHandler initPushEGPHandler, HostHandler initSparsePushEGPHandler) const
{
    // Generate reset kernel to be run before the neuron kernel
    const ModelSpecInternal &model = modelMerged.getModel();

    os << "//--------------------------------------------------------------------------" << std::endl;
    os << "// OpenCL program and kernels" << std::endl;
    os << "//--------------------------------------------------------------------------" << std::endl;
    os << "cl::Program initializeProgram;" << std::endl;
    os << "cl::Kernel " << KernelNames[KernelInitialize] << ";" << std::endl;
    os << "cl::Kernel " << KernelNames[KernelInitializeSparse] << ";" << std::endl;
    genMergedStructPreamble(os, modelMerged.getMergedNeuronInitGroups(), "NeuronInit");
    genMergedStructPreamble(os, modelMerged.getMergedSynapseDenseInitGroups(), "SynapseDenseInit");
    genMergedStructPreamble(os, modelMerged.getMergedSynapseConnectivityInitGroups(), "SynapseConnectivityInit");
    genMergedStructPreamble(os, modelMerged.getMergedSynapseSparseInitGroups(), "SynapseSparseInit");
    os << std::endl;

    // Generate preamble
    preambleHandler(os);

    // initialization kernel code
    size_t idInitStart = 0;

    //! KernelInitialize BODY START
    Substitutions kernelSubs(openclFunctions, model.getPrecision());

    // Creating the kernel body separately so it can be split into multiple string literals
    std::stringstream initializeKernelsStream;
    CodeStream initializeKernels(initializeKernelsStream);

    // Include definitions
    genKernelPreamble(initializeKernels, modelMerged);  

    // Generate struct definitions
    modelMerged.genMergedNeuronInitGroupStructs(initializeKernels, *this);
    modelMerged.genMergedSynapseDenseInitGroupStructs(initializeKernels, *this);
    modelMerged.genMergedSynapseConnectivityInitGroupStructs(initializeKernels, *this);
    modelMerged.genMergedSynapseSparseInitGroupStructs(initializeKernels, *this);

    // Generate data structure for accessing merged groups from within initialisation kernel
    // **NOTE** pass in zero constant cache here as it's precious and would be wasted on init kernels which are only launched once
    genMergedKernelDataStructures(initializeKernels, m_KernelWorkGroupSizes[KernelInitialize],
                                  modelMerged.getMergedNeuronInitGroups(), "NeuronInit",
                                  [](const NeuronGroupInternal &ng) { return ng.getNumNeurons(); },
                                  modelMerged.getMergedSynapseDenseInitGroups(), "SynapseDenseInit",
                                  [](const SynapseGroupInternal &sg) { return sg.getTrgNeuronGroup()->getNumNeurons(); },
                                  modelMerged.getMergedSynapseConnectivityInitGroups(), "SynapseConnectivityInit",
                                  [](const SynapseGroupInternal &sg) { return sg.getSrcNeuronGroup()->getNumNeurons(); });

    // Generate data structure for accessing merged groups from within sparse initialisation kernel
    genMergedKernelDataStructures(initializeKernels, m_KernelWorkGroupSizes[KernelInitializeSparse],
                                  modelMerged.getMergedSynapseSparseInitGroups(), "SynapseSparseInit",
                                  [](const SynapseGroupInternal &sg) { return sg.getMaxConnections(); });
    initializeKernels << std::endl;

    // Generate kernels used to populate merged structs
    genMergedStructBuildKernels(initializeKernels, modelMerged.getMergedNeuronInitGroups(), "NeuronInit");
    genMergedStructBuildKernels(initializeKernels, modelMerged.getMergedSynapseDenseInitGroups(), "SynapseDenseInit");
    genMergedStructBuildKernels(initializeKernels, modelMerged.getMergedSynapseConnectivityInitGroups(), "SynapseConnectivityInit");
    genMergedStructBuildKernels(initializeKernels, modelMerged.getMergedSynapseSparseInitGroups(), "SynapseSparseInit");

    initializeKernels << "__kernel void " << KernelNames[KernelInitialize] << "(";
    genMergedGroupKernelParams(initializeKernels, modelMerged.getMergedNeuronInitGroups(), "NeuronInit", true);
    genMergedGroupKernelParams(initializeKernels, modelMerged.getMergedSynapseDenseInitGroups(), "SynapseDenseInit", true);
    genMergedGroupKernelParams(initializeKernels, modelMerged.getMergedSynapseConnectivityInitGroups(), "SynapseConnectivityInit", true);
    initializeKernels << "unsigned int deviceRNGSeed";  // **TODO** check type
    initializeKernels << ")";
    {
        CodeStream::Scope b(initializeKernels);

        initializeKernels << "const unsigned int localId = get_local_id(0);" << std::endl;
        initializeKernels << "const unsigned int id = get_global_id(0);" << std::endl;

        initializeKernels << "// ------------------------------------------------------------------------" << std::endl;
        initializeKernels << "// Local neuron groups" << std::endl;
        // Parallelise over neuron groups
        genParallelGroup<NeuronInitGroupMerged>(initializeKernels, kernelSubs, modelMerged.getMergedNeuronInitGroups(), "NeuronInit", idInitStart,
            [this](const NeuronGroupInternal &ng) { return padSize(ng.getNumNeurons(), getKernelWorkGroupSize(KernelInitialize)); },
            [localNGHandler](CodeStream &os, const NeuronInitGroupMerged &ng, Substitutions &popSubs)
            {
                os << "// only do this for existing neurons" << std::endl;
                os << "if(" << popSubs["id"] << " < group->numNeurons)";
                {
                    CodeStream::Scope b(os);

                    //! TO BE IMPLEMENTED - isSimRNGRequired - isInitRNGRequired

                    localNGHandler(os, ng, popSubs);
                }
            });
        initializeKernels << std::endl;

        initializeKernels << "// ------------------------------------------------------------------------" << std::endl;
        initializeKernels << "// Synapse groups with dense connectivity" << std::endl;
        genParallelGroup<SynapseDenseInitGroupMerged>(initializeKernels, kernelSubs, modelMerged.getMergedSynapseDenseInitGroups(), "SynapseDenseInit", idInitStart,
            [this](const SynapseGroupInternal& sg) { return padSize(sg.getTrgNeuronGroup()->getNumNeurons(), getKernelWorkGroupSize(KernelInitialize)); },
            [sgDenseInitHandler](CodeStream& os, const SynapseDenseInitGroupMerged &sg, Substitutions &popSubs)
            {
                os << "// only do this for existing postsynaptic neurons" << std::endl;
                os << "if(" << popSubs["id"] << " < group->trgNumNeurons)";
                {
                    CodeStream::Scope b(os);

                    //! TO BE IMPLEMENTED - isWUInitRNGRequired

                    popSubs.addVarSubstitution("id_post", popSubs["id"]);
                    sgDenseInitHandler(os, sg, popSubs);
                }
            });
        initializeKernels << std::endl;

        initializeKernels << "// ------------------------------------------------------------------------" << std::endl;
        initializeKernels << "// Synapse groups with sparse connectivity" << std::endl;
        genParallelGroup<SynapseConnectivityInitGroupMerged>(initializeKernels, kernelSubs, modelMerged.getMergedSynapseConnectivityInitGroups(), "SynapseConnectivityInit", idInitStart,
            [this](const SynapseGroupInternal& sg) { return padSize(sg.getSrcNeuronGroup()->getNumNeurons(), getKernelWorkGroupSize(KernelInitialize)); },
            [sgSparseConnectHandler](CodeStream &os, const SynapseConnectivityInitGroupMerged &sg, Substitutions &popSubs)
            {
                os << "// only do this for existing presynaptic neurons" << std::endl;
                os << "if(" << popSubs["id"] << " < group->srcNumNeurons)";
                {
                    CodeStream::Scope b(os);

                    //! TO BE IMPLEMENTED - ::Utils::isRNGRequired

                    // If the synapse group has bitmask connectivity
                    if (sg.getArchetype().getMatrixType() & SynapseMatrixConnectivity::BITMASK) {
                        // Get maximum number of synapses anywhere in merged group
                        size_t maxSynapses = 0;
                        for(const auto &s : sg.getGroups()) {
                            maxSynapses = std::max(maxSynapses, (size_t)s.get().getTrgNeuronGroup()->getNumNeurons() * (size_t)s.get().getSrcNeuronGroup()->getNumNeurons());
                        }

                        // Calculate indices of bits at start and end of row
                        os << "// Calculate indices" << std::endl;
                        if ((maxSynapses & 0xFFFFFFFF00000000ULL) != 0) {
                            os << "const ulong rowStartGID = " << popSubs["id"] << " * group->trgNumNeurons;" << std::endl;
                        }
                        else {
                            os << "const unsigned int rowStartGID = " << popSubs["id"] << " * group->trgNumNeurons;" << std::endl;
                        }

                        // Build function template to set correct bit in bitmask
                        popSubs.addFuncSubstitution("addSynapse", 1,
                            "atomic_or(&group->gp[(rowStartGID + $(0)) / 32], 0x80000000 >> ((rowStartGID + $(0)) & 31))");
                    }
                    // Otherwise, if synapse group has ragged connectivity
                    else if (sg.getArchetype().getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
                        const std::string rowLength = "group->rowLength[" + popSubs["id"] + "]";
                        const std::string ind = "group->ind";

                        // Zero row length
                        os << rowLength << " = 0;" << std::endl;

                        // Build function template to increment row length and insert synapse into ind array
                        popSubs.addFuncSubstitution("addSynapse", 1,
                                                    "group->ind[(" + popSubs["id"] + " * group->rowStride) + (" + rowLength + "++)] = $(0)");
                    }
                    else {
                        assert(false);
                    }

                    popSubs.addVarSubstitution("id_pre", popSubs["id"]);
                    sgSparseConnectHandler(os, sg, popSubs);
                }
            });
    }
    const size_t numStaticInitThreads = idInitStart;

    // Generate sparse initialisation kernel
    size_t idSparseInitStart = 0;
    initializeKernels << "__kernel void " << KernelNames[KernelInitializeSparse] << "(";
    genMergedGroupKernelParams(initializeKernels, modelMerged.getMergedSynapseSparseInitGroups(), "SynapseSparseInit");
    initializeKernels << ")";
    {
        CodeStream::Scope b(initializeKernels);

        // Common variables for all cases
        Substitutions kernelSubs(openclFunctions, model.getPrecision());

        initializeKernels << "const unsigned int localId = get_local_id(0);" << std::endl;
        initializeKernels << "const unsigned int id = get_global_id(0);" << std::endl;

        // Shared memory array so row lengths don't have to be read by EVERY postsynaptic thread
        // **TODO** check actually required
        initializeKernels << "__local unsigned int shRowLength[" << m_KernelWorkGroupSizes[KernelInitializeSparse] << "];" << std::endl;
        if (std::any_of(modelMerged.getMergedSynapseSparseInitGroups().cbegin(), modelMerged.getMergedSynapseSparseInitGroups().cend(),
            [](const SynapseSparseInitGroupMerged &s) { return (s.getArchetype().getMatrixType() & SynapseMatrixConnectivity::SPARSE) && !s.getArchetype().getWUModel()->getSynapseDynamicsCode().empty(); }))
        {
            initializeKernels << "__local unsigned int shRowStart[" << m_KernelWorkGroupSizes[KernelInitializeSparse] + 1 << "];" << std::endl;
        }

        // Initialise weight update variables for synapse groups with dense connectivity
        genParallelGroup<SynapseSparseInitGroupMerged>(initializeKernels, kernelSubs, modelMerged.getMergedSynapseSparseInitGroups(), "SynapseSparseInit",  idSparseInitStart,
            [this](const SynapseGroupInternal &sg) { return padSize(sg.getMaxConnections(), m_KernelWorkGroupSizes[KernelInitializeSparse]); },
            [this, sgSparseInitHandler, numStaticInitThreads](CodeStream &os, const SynapseSparseInitGroupMerged  &sg, Substitutions& popSubs)
            {
                //! TO BE IMPLEMENTED - isWUInitRNGRequired

                os << "unsigned int idx = " << popSubs["id"] << ";" << std::endl;

                // Calculate how many blocks rows need to be processed in (in order to store row lengths in shared memory)
                const size_t workGroupSize = m_KernelWorkGroupSizes[KernelInitializeSparse];
                os << "const unsigned int numBlocks = (group->numSrcNeurons + " << workGroupSize << " - 1) / " << workGroupSize << ";" << std::endl;

                // Loop through blocks
                os << "for(unsigned int r = 0; r < numBlocks; r++)";
                {
                    CodeStream::Scope b(os);

                    // Calculate number of rows to process in this block
                    os << "const unsigned numRowsInBlock = (r == (numBlocks - 1))";
                    os << " ? ((group->numSrcNeurons - 1) % " << workGroupSize << ") + 1";
                    os << " : " << workGroupSize << ";" << std::endl;

                    // Use threads to copy block of sparse structure into shared memory
                    os << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;
                    os << "if (localId < numRowsInBlock)";
                    {
                        CodeStream::Scope b(os);
                        os << "shRowLength[localId] = group->rowLength[(r * " << workGroupSize << ") + localId];" << std::endl;
                    }

                    // If this synapse group has synapse dynamics
                    if (!sg.getArchetype().getWUModel()->getSynapseDynamicsCode().empty()) {
                        os << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;

                        // Use first thread to generate cumulative sum
                        os << "if (localId == 0)";
                        {
                            CodeStream::Scope b(os);

                            // Get index of last row in resultant synapse dynamics structure
                            // **NOTE** if there IS a previous block, it will always have had initSparseBlkSz rows in it
                            os << "unsigned int rowStart = (r == 0) ? 0 : shRowStart[" << workGroupSize << "];" << std::endl;
                            os << "shRowStart[0] = rowStart;" << std::endl;

                            // Loop through rows in block
                            os << "for(unsigned int i = 0; i < numRowsInBlock; i++)";
                            {
                                CodeStream::Scope b(os);

                                // Add this row's length to cumulative sum and write this to this row's end
                                os << "rowStart += shRowLength[i];" << std::endl;
                                os << "shRowStart[i + 1] = rowStart;" << std::endl;
                            }

                            // If this is the first thread block of the first block in the group AND the last block of rows,
                            // write the total cumulative sum to the first entry of the remap structure
                            os << "if(" << popSubs["id"] << " == 0 && (r == numBlocks - 1))";
                            {
                                CodeStream::Scope b(os);
                                os << "group->remap[0] = shRowStart[numRowsInBlock];" << std::endl;
                            }

                        }
                    }

                    os << "barrier(CLK_LOCAL_MEM_FENCE);" << std::endl;

                    // Loop through rows
                    os << "for(unsigned int i = 0; i < numRowsInBlock; i++)";
                    {
                        CodeStream::Scope b(os);

                        // If there is a synapse for this thread to initialise
                        os << "if(" << popSubs["id"] << " < shRowLength[i])";
                        {
                            CodeStream::Scope b(os);

                            // Generate sparse initialisation code
                            if (sg.getArchetype().isWUVarInitRequired()) {
                                popSubs.addVarSubstitution("id_pre", "((r * " + std::to_string(workGroupSize) + ") + i)");
                                popSubs.addVarSubstitution("id_post", "group->ind[idx]");

                                sgSparseInitHandler(os, sg, popSubs);
                            }

                            // If postsynaptic learning is required
                            if (!sg.getArchetype().getWUModel()->getLearnPostCode().empty()) {
                                CodeStream::Scope b(os);

                                // Extract index of synapse's postsynaptic target
                                os << "const unsigned int postIndex = group->ind[idx];" << std::endl;
                                
                                // Atomically increment length of column of connectivity associated with this target
                                // **NOTE** this returns previous length i.e. where to insert new entry
                                os << "const unsigned int colLocation = atomic_add(&group->colLength[postIndex], 1);" << std::endl;
                                
                                // From this calculate index into column-major matrix
                                os << "const unsigned int colMajorIndex = (postIndex * group->colStride) + colLocation;" << std::endl;

                                // Add remapping entry at this location poining back to row-major index
                                os << "group->remap[colMajorIndex] = idx;" << std::endl;
                            }

                            // If synapse dynamics are required, copy idx into syn remap structure
                            if (!sg.getArchetype().getWUModel()->getSynapseDynamicsCode().empty()) {
                                CodeStream::Scope b(os);
                                os << "remap->[shRowStart[i] + " + popSubs["id"] + " + 1] = idx;" << std::endl;
                            }
                        }

                        // If matrix is ragged, advance index to next row by adding stride
                        os << "idx += group->rowStride;" << std::endl;
                    }
                }
            });
        os << std::endl;
    }
    //! KernelInitializeSparse BODY END

    // Write out kernel source string literal
    os << "const char* initializeSrc = ";
    divideKernelStreamInParts(os, initializeKernelsStream, 5000);
    os << ";" << std::endl;
    os << std::endl;

    // Function for initializing the initialization kernels
    os << "// Initialize the initialization kernel(s)" << std::endl;
    os << "void buildInitializeProgram()";
    {
        CodeStream::Scope b(os);
        os << "// Build program" << std::endl;
        os << "initializeProgram = cl::Program(clContext, initializeSrc, true);" << std::endl;
        os << "if(initializeProgram.build(\"-cl-std=CL1.2\") != CL_SUCCESS)";
        {
            CodeStream::Scope b(os);
            os << "throw std::runtime_error(\"Compile error:\" + initializeProgram.getBuildInfo<CL_PROGRAM_BUILD_LOG>(clDevice));" << std::endl;
        }
        os << std::endl;

        os << "// Configure merged struct building kernels" << std::endl;
        genMergedStructBuild(os, modelMerged.getMergedNeuronInitGroups(), "NeuronInit", "initializeProgram");
        genMergedStructBuild(os, modelMerged.getMergedSynapseDenseInitGroups(), "SynapseDenseInit", "initializeProgram");
        genMergedStructBuild(os, modelMerged.getMergedSynapseDenseInitGroups(), "SynapseConnectivityInit", "initializeProgram");
        genMergedStructBuild(os, modelMerged.getMergedSynapseSparseInitGroups(), "SynapseSparseInit", "initializeProgram");
        os << std::endl;

        if (idInitStart > 0) {
            os << "// Configure initialization kernel" << std::endl;
            os << KernelNames[KernelInitialize] << " = cl::Kernel(initializeProgram, \"" << KernelNames[KernelInitialize] << "\");" << std::endl;
            setMergedGroupKernelParams(os, KernelNames[KernelInitialize], modelMerged.getMergedNeuronInitGroups(), "NeuronInit");
            setMergedGroupKernelParams(os, KernelNames[KernelInitialize], modelMerged.getMergedSynapseDenseInitGroups(), "SynapseDenseInit");
            setMergedGroupKernelParams(os, KernelNames[KernelInitialize], modelMerged.getMergedSynapseConnectivityInitGroups(), "SynapseConnectivityInit");
            os << std::endl;
        }

        if(idSparseInitStart > 0) {
            os << "// Configure sparse initialization kernel" << std::endl;
            os << KernelNames[KernelInitializeSparse] << " = cl::Kernel(initializeProgram, \"" << KernelNames[KernelInitializeSparse] << "\");" << std::endl;
            setMergedGroupKernelParams(os, KernelNames[KernelInitializeSparse], modelMerged.getMergedSynapseSparseInitGroups(), "SynapseSparseInit");
            os << std::endl;
        }
    }

    os << std::endl;

    os << "void initialize()";
    {
        CodeStream::Scope b(os);

        // If there are any initialisation work-items
        if (idInitStart > 0) {
            CodeStream::Scope b(os);
            //! TO BE IMPLEMENTED - Using hard coded deviceRNGSeed for now
            os << "unsigned int deviceRNGSeed = 0;" << std::endl;
            for (const auto& s : model.getSynapseGroups()) {
                // If this synapse population has BITMASK connectivity and is intialised on device, insert a call to cudaMemset to zero the whole bitmask
                if (s.second.isSparseConnectivityInitRequired() && s.second.getMatrixType() & SynapseMatrixConnectivity::BITMASK) {
                    const size_t gpSize = ((size_t)s.second.getSrcNeuronGroup()->getNumNeurons() * (size_t)s.second.getTrgNeuronGroup()->getNumNeurons()) / 32 + 1;
                    os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueFillBuffer(d_gp" << s.first << ", 0, 0, " << gpSize << " * sizeof(uint32_t)));" << std::endl;
                }
                // Otherwise, if this synapse population has RAGGED connectivity and has postsynaptic learning, insert a call to cudaMemset to zero column lengths
                else if ((s.second.getMatrixType() & SynapseMatrixConnectivity::SPARSE) && !s.second.getWUModel()->getLearnPostCode().empty()) {
                    os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueFillBuffer(d_colLength" << s.first << ", 0, 0, " << s.second.getTrgNeuronGroup()->getNumNeurons() << " * sizeof(unsigned int)));" << std::endl;
                }
            }
            os << std::endl;
            //os << "CHECK_OPENCL_ERRORS(" << KernelNames[KernelInitialize] << ".setArg(" << initializeKernelParams.size() /*last arg*/ << ", deviceRNGSeed));" << std::endl;
            os << std::endl;
            genKernelDimensions(os, KernelInitialize, idInitStart);
            const size_t numInitGroups = (modelMerged.getMergedNeuronInitGroups().size() + modelMerged.getMergedSynapseDenseInitGroups().size() + 
                                          modelMerged.getMergedSynapseConnectivityInitGroups().size());
            os << "CHECK_OPENCL_ERRORS(" << KernelNames[KernelInitialize] << ".setArg(" << numInitGroups << ", deviceRNGSeed));" << std::endl;
            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueNDRangeKernel(" << KernelNames[KernelInitialize] << ", cl::NullRange, globalWorkSize, localWorkSize));" << std::endl;
        }
    }

    os << std::endl;

    // Generating code for initializing all OpenCL elements - Using intializeSparse
    os << "// Initialize all OpenCL elements" << std::endl;
    os << "void initializeSparse()";
    {
        CodeStream::Scope b(os);
        // Copy all uninitialised state variables to device
        os << "copyStateToDevice(true);" << std::endl;
        os << "copyConnectivityToDevice(true);" << std::endl;

        // If there are any sparse initialisation work-items
        if (idSparseInitStart > 0) {
            CodeStream::Scope b(os);
            {
                genKernelDimensions(os, KernelInitializeSparse, idSparseInitStart);
                os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueNDRangeKernel(" << KernelNames[KernelInitializeSparse] << ", cl::NullRange, globalWorkSize, localWorkSize));" << std::endl;
            }
        }
    }
}
//--------------------------------------------------------------------------
size_t Backend::getSynapticMatrixRowStride(const SynapseGroupInternal &sg) const
{
    return getPresynapticUpdateStrategy(sg)->getSynapticMatrixRowStride(sg);
}
//--------------------------------------------------------------------------
void Backend::genDefinitionsPreamble(CodeStream& os, const ModelSpecMerged&) const
{
    os << "// Standard C++ includes" << std::endl;
    os << "#include <string>" << std::endl;
    os << "#include <stdexcept>" << std::endl;
    os << std::endl;
    os << "// Standard C includes" << std::endl;
    os << "#include <cstdint>" << std::endl;
    os << "#include <cassert>" << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genDefinitionsInternalPreamble(CodeStream& os, const ModelSpecMerged &) const
{
#ifdef _WIN32
    os << "#pragma warning(disable: 4297)" << std::endl;
#endif
    os << "// OpenCL includes" << std::endl;
    os << "#define CL_USE_DEPRECATED_OPENCL_1_2_APIS" << std::endl;
    os << "#include <CL/cl.hpp>" << std::endl;
    //os << "#include \"clRNG/lfsr113.h\"" << std::endl;
    os << std::endl;
    os << "// ------------------------------------------------------------------------" << std::endl;
    os << "// Helper macro for error-checking OpenCL calls" << std::endl;
    os << "#define CHECK_OPENCL_ERRORS(call) {\\" << std::endl;
    os << "    cl_int error = call;\\" << std::endl;
    os << "    if (error != CL_SUCCESS) {\\" << std::endl;
    os << "        throw std::runtime_error(__FILE__\": \" + std::to_string(__LINE__) + \": opencl error \" + std::to_string(error) + \": \" + clGetErrorString(error));\\" << std::endl;
    os << "    }\\" << std::endl;
    os << "}" << std::endl;

    os << std::endl;

    // Declaration of OpenCL functions
    os << "// ------------------------------------------------------------------------" << std::endl;
    os << "// OpenCL functions declaration" << std::endl;
    os << "// ------------------------------------------------------------------------" << std::endl;
    os << "const char* clGetErrorString(cl_int error);" << std::endl;

    os << std::endl;

    // Declaration of OpenCL variables
    os << "// OpenCL variables" << std::endl;
    os << "EXPORT_VAR cl::Context clContext;" << std::endl;
    os << "EXPORT_VAR cl::Device clDevice;" << std::endl;
    os << "EXPORT_VAR cl::CommandQueue commandQueue;" << std::endl;
    os << std::endl;

    os << "// OpenCL program initialization functions" << std::endl;
    os << "EXPORT_FUNC void buildInitializeProgram();" << std::endl;
    os << "EXPORT_FUNC void buildNeuronUpdateProgram();" << std::endl;
    os << "EXPORT_FUNC void buildSynapseUpdateProgram();" << std::endl;
    
    os << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genRunnerPreamble(CodeStream& os, const ModelSpecMerged &) const
{
    // Generating OpenCL variables for the runner
    os << "// OpenCL variables" << std::endl;
    os << "cl::Context clContext;" << std::endl;
    os << "cl::Device clDevice;" << std::endl;
    os << "cl::CommandQueue commandQueue;" << std::endl;
    os << std::endl;

    os << "// Get OpenCL error as string" << std::endl;
    os << "const char* clGetErrorString(cl_int error)";
    {
        CodeStream::Scope b(os);
        os << "switch(error)";
        {
            CodeStream::Scope b(os);

            #define STRINGIFY(ERR) #ERR
            #define GEN_CL_ERROR_CASE(ERR) os << "case " STRINGIFY(ERR) ": return \"" #ERR << "\";" << std::endl
            
            // run-time and JIT compiler errors
            GEN_CL_ERROR_CASE(CL_SUCCESS);
            GEN_CL_ERROR_CASE(CL_DEVICE_NOT_FOUND);
            GEN_CL_ERROR_CASE(CL_DEVICE_NOT_AVAILABLE);
            GEN_CL_ERROR_CASE(CL_COMPILER_NOT_AVAILABLE);
            GEN_CL_ERROR_CASE(CL_MEM_OBJECT_ALLOCATION_FAILURE);
            GEN_CL_ERROR_CASE(CL_OUT_OF_RESOURCES);
            GEN_CL_ERROR_CASE(CL_OUT_OF_HOST_MEMORY);
            GEN_CL_ERROR_CASE(CL_PROFILING_INFO_NOT_AVAILABLE);
            GEN_CL_ERROR_CASE(CL_MEM_COPY_OVERLAP);
            GEN_CL_ERROR_CASE(CL_IMAGE_FORMAT_MISMATCH);
            GEN_CL_ERROR_CASE(CL_IMAGE_FORMAT_NOT_SUPPORTED);
            GEN_CL_ERROR_CASE(CL_BUILD_PROGRAM_FAILURE);
            GEN_CL_ERROR_CASE(CL_MAP_FAILURE);
            GEN_CL_ERROR_CASE(CL_MISALIGNED_SUB_BUFFER_OFFSET);
            GEN_CL_ERROR_CASE(CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST);
            GEN_CL_ERROR_CASE(CL_COMPILE_PROGRAM_FAILURE);
            GEN_CL_ERROR_CASE(CL_LINKER_NOT_AVAILABLE);
            GEN_CL_ERROR_CASE(CL_LINK_PROGRAM_FAILURE);
            GEN_CL_ERROR_CASE(CL_DEVICE_PARTITION_FAILED);
            GEN_CL_ERROR_CASE(CL_KERNEL_ARG_INFO_NOT_AVAILABLE);

            // compile-time errors
            GEN_CL_ERROR_CASE(CL_INVALID_VALUE);
            GEN_CL_ERROR_CASE(CL_INVALID_DEVICE_TYPE);
            GEN_CL_ERROR_CASE(CL_INVALID_PLATFORM);
            GEN_CL_ERROR_CASE(CL_INVALID_DEVICE);
            GEN_CL_ERROR_CASE(CL_INVALID_CONTEXT);
            GEN_CL_ERROR_CASE(CL_INVALID_QUEUE_PROPERTIES);
            GEN_CL_ERROR_CASE(CL_INVALID_COMMAND_QUEUE);
            GEN_CL_ERROR_CASE(CL_INVALID_HOST_PTR);
            GEN_CL_ERROR_CASE(CL_INVALID_MEM_OBJECT);
            GEN_CL_ERROR_CASE(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR);
            GEN_CL_ERROR_CASE(CL_INVALID_IMAGE_SIZE);
            GEN_CL_ERROR_CASE(CL_INVALID_SAMPLER);
            GEN_CL_ERROR_CASE(CL_INVALID_BINARY);
            GEN_CL_ERROR_CASE(CL_INVALID_BUILD_OPTIONS);
            GEN_CL_ERROR_CASE(CL_INVALID_PROGRAM);
            GEN_CL_ERROR_CASE(CL_INVALID_PROGRAM_EXECUTABLE);
            GEN_CL_ERROR_CASE(CL_INVALID_KERNEL_NAME);
            GEN_CL_ERROR_CASE(CL_INVALID_KERNEL_DEFINITION);
            GEN_CL_ERROR_CASE(CL_INVALID_KERNEL);
            GEN_CL_ERROR_CASE(CL_INVALID_ARG_INDEX);
            GEN_CL_ERROR_CASE(CL_INVALID_ARG_VALUE);
            GEN_CL_ERROR_CASE(CL_INVALID_ARG_SIZE);
            GEN_CL_ERROR_CASE(CL_INVALID_KERNEL_ARGS);
            GEN_CL_ERROR_CASE(CL_INVALID_WORK_DIMENSION);
            GEN_CL_ERROR_CASE(CL_INVALID_WORK_GROUP_SIZE);
            GEN_CL_ERROR_CASE(CL_INVALID_WORK_ITEM_SIZE);
            GEN_CL_ERROR_CASE(CL_INVALID_GLOBAL_OFFSET);
            GEN_CL_ERROR_CASE(CL_INVALID_EVENT_WAIT_LIST);
            GEN_CL_ERROR_CASE(CL_INVALID_EVENT);
            GEN_CL_ERROR_CASE(CL_INVALID_OPERATION);
            GEN_CL_ERROR_CASE(CL_INVALID_GL_OBJECT);
            GEN_CL_ERROR_CASE(CL_INVALID_BUFFER_SIZE);
            GEN_CL_ERROR_CASE(CL_INVALID_MIP_LEVEL);
            GEN_CL_ERROR_CASE(CL_INVALID_GLOBAL_WORK_SIZE);
            GEN_CL_ERROR_CASE(CL_INVALID_PROPERTY);
            GEN_CL_ERROR_CASE(CL_INVALID_IMAGE_DESCRIPTOR);
            GEN_CL_ERROR_CASE(CL_INVALID_COMPILER_OPTIONS);
            GEN_CL_ERROR_CASE(CL_INVALID_LINKER_OPTIONS);
            GEN_CL_ERROR_CASE(CL_INVALID_DEVICE_PARTITION_COUNT);
            os << "default: return \"Unknown OpenCL error\";" << std::endl;
            
            #undef GEN_CL_ERROR_CASE
            #undef STRINGIFY
        }
    }
    os << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genAllocateMemPreamble(CodeStream& os, const ModelSpecMerged&) const
{
    // Initializing OpenCL programs
    os << "// Get platforms" << std::endl;
    os << "std::vector<cl::Platform> platforms; " << std::endl;
    os << "cl::Platform::get(&platforms);" << std::endl;
    
    os << "// Get platform devices" << std::endl;
    os << "std::vector<cl::Device> platformDevices; " << std::endl;
    os << "platforms[" << m_ChosenPlatformIndex << "].getDevices(CL_DEVICE_TYPE_ALL, &platformDevices);" << std::endl;
    
    os << "// Select device and create context and command queue" << std::endl;
    os << "clDevice = platformDevices[" << m_ChosenDeviceIndex << "];" << std::endl;
    os << "clContext = cl::Context(clDevice);" << std::endl;
    os << "commandQueue = cl::CommandQueue(clContext, clDevice);" << std::endl;

    os << "// Build OpenCL programs" << std::endl;
    os << "buildInitializeProgram();" << std::endl;
    os << "buildNeuronUpdateProgram();" << std::endl;
    os << "buildSynapseUpdateProgram();" << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genStepTimeFinalisePreamble(CodeStream& os, const ModelSpecMerged&) const
{
    printf("TO BE IMPLEMENTED: ~virtual~ CodeGenerator::OpenCL::Backend::genStepTimeFinalisePreamble\n");
}
//--------------------------------------------------------------------------
void Backend::genVariableDefinition(CodeStream& definitions, CodeStream& definitionsInternal, const std::string& type, const std::string& name, VarLocation loc) const
{
    const bool deviceType = isDeviceType(type);

    if (loc & VarLocation::HOST) {
        if (deviceType) {
            throw std::runtime_error("Variable '" + name + "' is of device-only type '" + type + "' but is located on the host");
        }
        definitions << "EXPORT_VAR " << type << " " << name << ";" << std::endl;
    }
    if (loc & VarLocation::DEVICE) {
        definitionsInternal << "EXPORT_VAR cl::Buffer d_" << name << ";" << std::endl;
    }
}
//--------------------------------------------------------------------------
void Backend::genVariableImplementation(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc) const
{
    if (loc & VarLocation::HOST) {
        os << type << " " << name << ";" << std::endl;
    }
    if (loc & VarLocation::DEVICE) {
        os << "cl::Buffer d_" << name << ";" << std::endl;
    }
}
//--------------------------------------------------------------------------
MemAlloc Backend::genVariableAllocation(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc, size_t count) const
{
    auto allocation = MemAlloc::zero();

    if (loc & VarLocation::HOST) {
        os << name << " = new " << type << "[" << count << "];" << std::endl;
        allocation += MemAlloc::host(count * getSize(type));
    }

    // If variable is present on device then initialize the device buffer
    if (loc & VarLocation::DEVICE) {
        os << "d_" << name << " = cl::Buffer(clContext, CL_MEM_READ_WRITE, " << count << " * sizeof(" << type << "), ";
        if(loc & VarLocation::HOST) {
            os << name << ");" << std::endl;
        }
        else {
            os << "nullptr);" << std::endl;
        }
        
        allocation += MemAlloc::device(count * getSize(type));
    }

    return allocation;
}
//--------------------------------------------------------------------------
void Backend::genVariableFree(CodeStream& os, const std::string& name, VarLocation loc) const
{
    if (loc & VarLocation::HOST) {
        os << "delete[] " << name << ";" << std::endl;
    }
}
//--------------------------------------------------------------------------
void Backend::genExtraGlobalParamDefinition(CodeStream& definitions, const std::string& type, const std::string& name, VarLocation loc) const
{
    if (loc & VarLocation::HOST) {
        definitions << "EXPORT_VAR " << type << " " << name << ";" << std::endl;
    }
    if (loc & VarLocation::DEVICE && ::Utils::isTypePointer(type)) {
        definitions << "EXPORT_VAR " << type << " d_" << name << ";" << std::endl;
    }
}
//--------------------------------------------------------------------------
void Backend::genExtraGlobalParamImplementation(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc) const
{
    if (loc & VarLocation::HOST) {
        os << type << " " << name << ";" << std::endl;
    }
    if (loc & VarLocation::DEVICE && ::Utils::isTypePointer(type)) {
        os << type << " d_" << name << ";" << std::endl;
    }
}
//--------------------------------------------------------------------------
void Backend::genExtraGlobalParamAllocation(CodeStream &os, const std::string &type, const std::string &name,
                                            VarLocation loc, const std::string &countVarName, const std::string &prefix) const
{
    // Get underlying type
    const std::string underlyingType = ::Utils::getUnderlyingType(type);
    const bool pointerToPointer = ::Utils::isTypePointerToPointer(type);

    const std::string hostPointer = pointerToPointer ? ("*" + prefix + name) : (prefix + name);
    const std::string devicePointer = pointerToPointer ? ("*" + prefix + "d_" + name) : (prefix + "d_" + name);

    if(loc & VarLocation::HOST) {
        os << hostPointer << " = new " << underlyingType << "[" << countVarName << "];" << std::endl;
    }

    // If variable is present on device at all
    if(loc & VarLocation::DEVICE) {
        os << devicePointer << " = cl::Buffer(clContext, CL_MEM_READ_WRITE, " << countVarName << " * sizeof(" << underlyingType << "), ";
    }

    /*if (loc & VarLocation::HOST) {
        os << name << " = (" << underlyingType << "*)calloc(count, sizeof(" << underlyingType << "));" << std::endl;
    }

    // If variable is present on device at all
    if (loc & VarLocation::DEVICE) {
        os << getVarPrefix() << name << " = (" << underlyingType << "*)calloc(count, sizeof(" << underlyingType << "));" << std::endl;
    }*/
}
//--------------------------------------------------------------------------
void Backend::genExtraGlobalParamPush(CodeStream &os, const std::string &type, const std::string &name,
                                      VarLocation loc, const std::string &countVarName, const std::string &prefix) const
{
    if (!(loc & VarLocation::ZERO_COPY)) {
        throw Utils::ToBeImplemented("genExtraGlobalParamPush");
        //! TO BE REVIEWED - No need to push
    }
}
//--------------------------------------------------------------------------
void Backend::genExtraGlobalParamPull(CodeStream &os, const std::string &type, const std::string &name,
                                      VarLocation loc, const std::string &countVarName, const std::string &prefix) const
{
    if (!(loc & VarLocation::ZERO_COPY)) {
        throw Utils::ToBeImplemented("genExtraGlobalParamPull");
    }
}
//--------------------------------------------------------------------------
void Backend::genMergedExtraGlobalParamPush(CodeStream &os, const std::string &suffix, size_t mergedGroupIdx,
                                            const std::string &groupIdx, const std::string &fieldName,
                                            const std::string &egpName) const
{
    const std::string structName = "Merged" + suffix + "Group" + std::to_string(mergedGroupIdx);
    os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueWriteBuffer(dd_merged" << suffix << "Group" << mergedGroupIdx;
    os << ", " << "CL_FALSE";
    os << ", " << "(sizeof(" << structName << ") * (" << groupIdx << ")) + offsetof(" << structName << ", " << fieldName << ")";
    os << ", " << "sizeof(" << egpName << ")";
    os << ", &egpName));" << std::endl;
}
//--------------------------------------------------------------------------
std::string Backend::getMergedGroupFieldHostType(const std::string &type) const
{
    // If type is a pointer, on the host it is represented by an OpenCL buffer
    if(::Utils::isTypePointer(type)) {
        return "cl::Buffer";
    }
    // Otherwise, type remains the same
    else {
        return type;
    }
}
//--------------------------------------------------------------------------
void Backend::genPopVariableInit(CodeStream& os, const Substitutions& kernelSubs, Handler handler) const
{
    Substitutions varSubs(&kernelSubs);

    // If this is first thread in group
    os << "if(" << varSubs["id"] << " == 0)";
    {
        CodeStream::Scope b(os);
        handler(os, varSubs);
    }
}
//--------------------------------------------------------------------------
void Backend::genVariableInit(CodeStream &os, const std::string &, const std::string &countVarName,
                              const Substitutions &kernelSubs, Handler handler) const
{
    // Variable should already be provided via parallelism
    assert(kernelSubs.hasVarSubstitution(countVarName));

    Substitutions varSubs(&kernelSubs);
    handler(os, varSubs);
}
//--------------------------------------------------------------------------
void Backend::genSynapseVariableRowInit(CodeStream &os, const SynapseGroupMergedBase &,
                                        const Substitutions &kernelSubs, Handler handler) const
{
    // Pre and postsynaptic ID should already be provided via parallelism
    assert(kernelSubs.hasVarSubstitution("id_pre"));
    assert(kernelSubs.hasVarSubstitution("id_post"));

    Substitutions varSubs(&kernelSubs);
    varSubs.addVarSubstitution("id_syn", "(" + kernelSubs["id_pre"] + " * group->rowStride) + " + kernelSubs["id"]);
    handler(os, varSubs);
}
//--------------------------------------------------------------------------
void Backend::genVariablePush(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc, bool autoInitialized, size_t count) const
{
    if (!(loc & VarLocation::ZERO_COPY)) {
        // Only copy if uninitialisedOnly isn't set
        if (autoInitialized) {
            os << "if(!uninitialisedOnly)" << CodeStream::OB(1101);
        }

        os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueWriteBuffer(d_" << name;
        os << ", " << "CL_TRUE";
        os << ", " << "0";
        os << ", " << count << " * sizeof(" << type << ")";
        os << ", " << name << "));" << std::endl;

        if (autoInitialized) {
            os << CodeStream::CB(1101);
        }
    }
}
//--------------------------------------------------------------------------
void Backend::genVariablePull(CodeStream& os, const std::string& type, const std::string& name, VarLocation loc, size_t count) const
{
    if (!(loc & VarLocation::ZERO_COPY)) {
        os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueReadBuffer(d_" << name;
        os << ", " << "CL_TRUE";
        os << ", " << "0";
        os << ", " << count << " * sizeof(" << type << ")";
        os << ", " << name << "));" << std::endl;
    }
}
//--------------------------------------------------------------------------
void Backend::genCurrentVariablePush(CodeStream& os, const NeuronGroupInternal& ng, const std::string& type, const std::string& name, VarLocation loc) const
{
    // If this variable requires queuing and isn't zero-copy
    if (ng.isVarQueueRequired(name) && ng.isDelayRequired() && !(loc & VarLocation::ZERO_COPY)) {
        // Generate memcpy to copy only current timestep's data
        //! TO BE IMPLEMENTED - Current push not applicable for OpenCL
        /*
        os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueWriteBuffer(" << getVarPrefix() << name << ng.getName();
        os << "[spkQuePtr" << ng.getName() << " * " << ng.getNumNeurons() << "]";
        os << ", " << "CL_TRUE";
        os << ", " << "0";
        os << ", " << ng.getNumNeurons() << " * sizeof(" << type << ")";
        os << ", &" << name << ng.getName() << "[spkQuePtr" << ng.getName() << " * " << ng.getNumNeurons() << "]));" << std::endl;
        */
        genVariablePush(os, type, name + ng.getName(), loc, false, ng.getNumNeurons());
    }
    // Otherwise, generate standard push
    else {
        genVariablePush(os, type, name + ng.getName(), loc, false, ng.getNumNeurons());
    }
}
//--------------------------------------------------------------------------
void Backend::genCurrentVariablePull(CodeStream& os, const NeuronGroupInternal& ng, const std::string& type, const std::string& name, VarLocation loc) const
{
    // If this variable requires queuing and isn't zero-copy
    if (ng.isVarQueueRequired(name) && ng.isDelayRequired() && !(loc & VarLocation::ZERO_COPY)) {
        // Generate memcpy to copy only current timestep's data
        //! TO BE IMPLEMENTED - Current pull not applicable for OpenCL
        /*
        os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueReadBuffer(" << getVarPrefix() << name << ng.getName();
        os << "[spkQuePtr" << ng.getName() << " * " << ng.getNumNeurons() << "]";
        os << ", " << "CL_TRUE";
        os << ", " << "0";
        os << ", " << ng.getNumNeurons() << " * sizeof(" << type << ")";
        os << ", &" << name << ng.getName() << "[spkQuePtr" << ng.getName() << " * " << ng.getNumNeurons() << "]));" << std::endl;
        */
        genVariablePull(os, type, name + ng.getName(), loc, ng.getNumNeurons());
    }
    // Otherwise, generate standard push
    else {
        genVariablePull(os, type, name + ng.getName(), loc, ng.getNumNeurons());
    }
}
//--------------------------------------------------------------------------
MemAlloc Backend::genGlobalDeviceRNG(CodeStream &, CodeStream &definitionsInternal, CodeStream &runner, CodeStream &, CodeStream &) const
{
    throw Utils::ToBeImplemented("genGlobalDeviceRNG");
    return MemAlloc::zero();
}
//--------------------------------------------------------------------------
MemAlloc Backend::genPopulationRNG(CodeStream& definitions, CodeStream& definitionsInternal, CodeStream& runner, CodeStream& allocations, CodeStream& free,
    const std::string& name, size_t count) const
{
    genVariableDefinition(definitionsInternal, definitionsInternal, "clrngLfsr113Stream*", name, VarLocation::HOST_DEVICE);
    genVariableImplementation(runner, "clrngLfsr113Stream*", name, VarLocation::HOST_DEVICE);
    genVariableFree(free, name, VarLocation::HOST_DEVICE);

    // genVariableAllocation
    auto allocation = MemAlloc::zero();

    allocations << name << " = clrngLfsr113CreateStreams(NULL, " << count << ", &" << name << "Count, NULL);" << std::endl;
    allocations << "d_" << name << " = cl::Buffer(clContext, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, " << count << ", " << name << ");" << std::endl;

    return allocation;
}
//--------------------------------------------------------------------------
void Backend::genTimer(CodeStream&, CodeStream& definitionsInternal, CodeStream& runner, CodeStream& allocations, CodeStream& free,
    CodeStream& stepTimeFinalise, const std::string& name, bool updateInStepTime) const
{
    throw Utils::ToBeImplemented("genTimer");
}
//--------------------------------------------------------------------------
void Backend::genReturnFreeDeviceMemoryBytes(CodeStream &os) const
{
    // **NOTE** OpenCL does not have this functionality
    os << "return 0;" << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genMakefilePreamble(std::ostream& os) const
{
    os << "LIBS := " << "-lOpenCL" << std::endl;
    os << "INCL := " << "-I$(OPENCL_PATH)/include" << std::endl;
    os << "LINKFLAGS := " << "-shared" << std::endl;
    os << "CXXFLAGS := " << "-c -fPIC -std=c++11 -MMD -MP" << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genMakefileLinkRule(std::ostream& os) const
{
    os << "\t@$(CXX) $(LINKFLAGS) -o $@ $(OBJECTS) $(LIBS)" << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genMakefileCompileRule(std::ostream& os) const
{
    os << "%.o: %.cc" << std::endl;
    os << "\t@$(CXX) $(CXXFLAGS) $(INCL) -o $@ $<" << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genMSBuildConfigProperties(std::ostream& os) const
{
}
//--------------------------------------------------------------------------
void Backend::genMSBuildImportProps(std::ostream& os) const
{
    os << "\t<ImportGroup Label=\"ExtensionSettings\">" << std::endl;
    os << "\t</ImportGroup>" << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genMSBuildItemDefinitions(std::ostream& os) const
{
    // Add item definition for host compilation
    os << "\t\t<ClCompile>" << std::endl;
    os << "\t\t\t<WarningLevel>Level3</WarningLevel>" << std::endl;
    os << "\t\t\t<Optimization Condition=\"'$(Configuration)'=='Release'\">MaxSpeed</Optimization>" << std::endl;
    os << "\t\t\t<Optimization Condition=\"'$(Configuration)'=='Debug'\">Disabled</Optimization>" << std::endl;
    os << "\t\t\t<FunctionLevelLinking Condition=\"'$(Configuration)'=='Release'\">true</FunctionLevelLinking>" << std::endl;
    os << "\t\t\t<IntrinsicFunctions Condition=\"'$(Configuration)'=='Release'\">true</IntrinsicFunctions>" << std::endl;
    os << "\t\t\t<PreprocessorDefinitions Condition=\"'$(Configuration)'=='Release'\">WIN32;WIN64;NDEBUG;_CONSOLE;BUILDING_GENERATED_CODE;%(PreprocessorDefinitions)</PreprocessorDefinitions>" << std::endl;
    os << "\t\t\t<PreprocessorDefinitions Condition=\"'$(Configuration)'=='Debug'\">WIN32;WIN64;_DEBUG;_CONSOLE;BUILDING_GENERATED_CODE;%(PreprocessorDefinitions)</PreprocessorDefinitions>" << std::endl;
    os << "\t\t\t<AdditionalIncludeDirectories>..\\clRNG\\include;$(OPENCL_PATH)\\include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>" << std::endl;
    os << "\t\t</ClCompile>" << std::endl;

    // Add item definition for linking
    os << "\t\t<Link>" << std::endl;
    os << "\t\t\t<GenerateDebugInformation>true</GenerateDebugInformation>" << std::endl;
    os << "\t\t\t<EnableCOMDATFolding Condition=\"'$(Configuration)'=='Release'\">true</EnableCOMDATFolding>" << std::endl;
    os << "\t\t\t<OptimizeReferences Condition=\"'$(Configuration)'=='Release'\">true</OptimizeReferences>" << std::endl;
    os << "\t\t\t<SubSystem>Console</SubSystem>" << std::endl;
    os << "\t\t\t<AdditionalLibraryDirectories>$(OPENCL_PATH)\\lib\\x64;$(OPENCL_PATH)\\lib\\x86_64;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>" << std::endl;
    os << "\t\t\t<AdditionalDependencies>OpenCL.lib;kernel32.lib;user32.lib;gdi32.lib;winspool.lib;comdlg32.lib;advapi32.lib;shell32.lib;ole32.lib;oleaut32.lib;uuid.lib;odbc32.lib;odbccp32.lib;%(AdditionalDependencies)</AdditionalDependencies>" << std::endl;
    os << "\t\t</Link>" << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genMSBuildCompileModule(const std::string& moduleName, std::ostream& os) const
{
    os << "\t\t<ClCompile Include=\"" << moduleName << ".cc\" />" << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genMSBuildImportTarget(std::ostream&) const
{
}
//--------------------------------------------------------------------------
std::string Backend::getFloatAtomicAdd(const std::string& ftype, const char* memoryType) const
{
    if (ftype == "float" || ftype == "double") {
        return "atomic_add_f_" + std::string(memoryType);
    }
    else {
        return "atomic_add";
    }
}
//--------------------------------------------------------------------------
size_t Backend::getNumPresynapticUpdateThreads(const SynapseGroupInternal &sg)
{
     return getPresynapticUpdateStrategy(sg)->getNumThreads(sg);
}
//--------------------------------------------------------------------------
size_t Backend::getNumPostsynapticUpdateThreads(const SynapseGroupInternal &sg)
{
    if (sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
        return sg.getMaxSourceConnections();
    }
    else {
        return sg.getSrcNeuronGroup()->getNumNeurons();
    }
}
//--------------------------------------------------------------------------
size_t Backend::getNumSynapseDynamicsThreads(const SynapseGroupInternal &sg)
{
    if (sg.getMatrixType() & SynapseMatrixConnectivity::SPARSE) {
        return (size_t)sg.getSrcNeuronGroup()->getNumNeurons() * sg.getMaxConnections();
    }
    else {
        return (size_t)sg.getSrcNeuronGroup()->getNumNeurons() * sg.getTrgNeuronGroup()->getNumNeurons();
    }
}
//--------------------------------------------------------------------------
void Backend::addPresynapticUpdateStrategy(PresynapticUpdateStrategy::Base *strategy)
{
    s_PresynapticUpdateStrategies.push_back(strategy);
}
//--------------------------------------------------------------------------
bool Backend::isGlobalHostRNGRequired(const ModelSpecMerged &modelMerged) const
{
    // Host RNG is required if any synapse groups require a host initialization RNG
    const ModelSpecInternal &model = modelMerged.getModel();
    return std::any_of(model.getSynapseGroups().cbegin(), model.getSynapseGroups().cend(),
                       [](const ModelSpec::SynapseGroupValueType &s)
    {
        return (s.second.isHostInitRNGRequired());
    });
}
//--------------------------------------------------------------------------
bool Backend::isGlobalDeviceRNGRequired(const ModelSpecMerged &modelMerged) const
{
    // If any neuron groups require  RNG for initialisation, return true
    // **NOTE** this takes postsynaptic model initialisation into account
    const ModelSpecInternal &model = modelMerged.getModel();
    if(std::any_of(model.getNeuronGroups().cbegin(), model.getNeuronGroups().cend(),
                   [](const ModelSpec::NeuronGroupValueType &n)
    {
        return n.second.isInitRNGRequired();
    }))
    {
        return true;
    }

    // If any synapse groups require an RNG for weight update model initialisation or procedural connectivity, return true
    if(std::any_of(model.getSynapseGroups().cbegin(), model.getSynapseGroups().cend(),
                   [](const ModelSpec::SynapseGroupValueType &s)
    {
        return (s.second.isWUInitRNGRequired() || s.second.isProceduralConnectivityRNGRequired());
    }))
    {
        return true;
    }

    return false;
}
//--------------------------------------------------------------------------
Backend::MemorySpaces Backend::getMergedGroupMemorySpaces(const ModelSpecMerged &) const
{
    return {};
}
//--------------------------------------------------------------------------
void Backend::genCurrentSpikePush(CodeStream& os, const NeuronGroupInternal& ng, bool spikeEvent) const
{
    if (!(ng.getSpikeLocation() & VarLocation::ZERO_COPY)) {
        // Is delay required
        const bool delayRequired = spikeEvent ?
            ng.isDelayRequired() :
            (ng.isTrueSpikeRequired() && ng.isDelayRequired());

        const char* spikeCntPrefix = spikeEvent ? "glbSpkCntEvnt" : "glbSpkCnt";
        const char* spikePrefix = spikeEvent ? "glbSpkEvnt" : "glbSpk";

        if (delayRequired) {
            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueWriteBuffer(d_" << spikeCntPrefix << ng.getName();
            os << ", " << "CL_TRUE";
            os << ", " << "0";
            os << ", sizeof(unsigned int)";
            os << ", " << spikeCntPrefix << ng.getName() << "));" << std::endl;

            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueWriteBuffer(d_" << spikePrefix << ng.getName();
            os << ", " << "CL_TRUE";
            os << ", " << "0";
            os << ", " << ng.getNumNeurons() << " * sizeof(unsigned int)";
            os << ", " << spikePrefix << ng.getName() << "));" << std::endl;
        }
        else {
            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueWriteBuffer(d_" << spikeCntPrefix << ng.getName();
            os << ", " << "CL_TRUE";
            os << ", " << "0";
            os << ", sizeof(unsigned int)";
            os << ", " << spikeCntPrefix << ng.getName() << "));" << std::endl;

            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueWriteBuffer(d_" << spikePrefix << ng.getName();
            os << ", " << "CL_TRUE";
            os << ", " << "0";
            os << ", " << spikeCntPrefix << ng.getName() << "[0] * sizeof(unsigned int)";
            os << ", " << spikePrefix << ng.getName() << "));" << std::endl;
        }
    }
}
//--------------------------------------------------------------------------
void Backend::genCurrentSpikePull(CodeStream& os, const NeuronGroupInternal& ng, bool spikeEvent) const
{
    if (!(ng.getSpikeLocation() & VarLocation::ZERO_COPY)) {
        // Is delay required
        const bool delayRequired = spikeEvent ?
            ng.isDelayRequired() :
            (ng.isTrueSpikeRequired() && ng.isDelayRequired());

        const char* spikeCntPrefix = spikeEvent ? "glbSpkCntEvnt" : "glbSpkCnt";
        const char* spikePrefix = spikeEvent ? "glbSpkEvnt" : "glbSpk";

        if (delayRequired) {
            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueReadBuffer(d_" << spikeCntPrefix << ng.getName();
            os << ", " << "CL_TRUE";
            os << ", " << "0";
            os << ", sizeof(unsigned int)";
            os << ", " << spikeCntPrefix << ng.getName() << "));" << std::endl;

            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueReadBuffer(d_" << spikePrefix << ng.getName();
            os << ", " << "CL_TRUE";
            os << ", " << "0";
            os << ", " << ng.getNumNeurons() << " * sizeof(unsigned int)";
            os << ", " << spikePrefix << ng.getName() << "));" << std::endl;
        }
        else {
            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueReadBuffer(d_" << spikeCntPrefix << ng.getName();
            os << ", " << "CL_TRUE";
            os << ", " << "0";
            os << ", sizeof(unsigned int)";
            os << ", " << spikeCntPrefix << ng.getName() << "));" << std::endl;

            os << "CHECK_OPENCL_ERRORS(commandQueue.enqueueReadBuffer(d_" << spikePrefix << ng.getName();
            os << ", " << "CL_TRUE";
            os << ", " << "0";
            os << ", " << spikeCntPrefix << ng.getName() << "[0] * sizeof(unsigned int)";
            os << ", " << spikePrefix << ng.getName() << "));" << std::endl;
        }
    }
}
//--------------------------------------------------------------------------
void Backend::genEmitSpike(CodeStream& os, const Substitutions& subs, const std::string& suffix) const
{
    os << "const unsigned int spk" << suffix << "Idx = atomic_add(&shSpk" << suffix << "Count, 1);" << std::endl;
    os << "shSpk" << suffix << "[spk" << suffix << "Idx] = " << subs["id"] << ";" << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genKernelDimensions(CodeStream& os, Kernel kernel, size_t numThreads) const
{
    // Calculate global and local work size
    const size_t numOfWorkGroups = ceilDivide(numThreads, m_KernelWorkGroupSizes[kernel]);
    os << "const cl::NDRange globalWorkSize(" << (m_KernelWorkGroupSizes[kernel] * numOfWorkGroups) << ", 1);" << std::endl;
    os << "const cl::NDRange localWorkSize(" << m_KernelWorkGroupSizes[kernel] << ", 1);" << std::endl;
}
//--------------------------------------------------------------------------
void Backend::genKernelPreamble(CodeStream &os, const ModelSpecMerged &modelMerged) const
{
    const ModelSpecInternal &model = modelMerged.getModel();
    os << "typedef " << model.getPrecision() << " scalar;" << std::endl;
    os << "#define DT " << std::to_string(model.getDT());
    if(model.getTimePrecision() == "float") {
        os << "f";
    } 
    os << std::endl;
    genTypeRange(os, model.getTimePrecision(), "TIME");

    // **YUCK** OpenCL doesn't let you include C99 system header so, instead, 
    // manually define C99 types in terms of OpenCL types (whose sizes are guaranteed)
    os << "// ------------------------------------------------------------------------" << std::endl;
    os << "// C99 sized types" << std::endl;
    os << "typedef uchar uint8_t;" << std::endl;
    os << "typedef ushort uint16_t;" << std::endl;
    os << "typedef uint uint32_t;" << std::endl;
    os << "typedef char int8_t;" << std::endl;
    os << "typedef short int16_t;" << std::endl;
    os << "typedef int int32_t;" << std::endl;
}
//--------------------------------------------------------------------------
void Backend::addDeviceType(const std::string& type, size_t size)
{
    addType(type, size);
    m_DeviceTypes.emplace(type);
}
//--------------------------------------------------------------------------
bool Backend::isDeviceType(const std::string& type) const
{
    // Get underlying type
    const std::string underlyingType = ::Utils::isTypePointer(type) ? ::Utils::getUnderlyingType(type) : type;

    // Return true if it is in device types set
    return (m_DeviceTypes.find(underlyingType) != m_DeviceTypes.cend());
}
//--------------------------------------------------------------------------
void Backend::divideKernelStreamInParts(CodeStream& os, const std::stringstream& kernelCode, size_t partLength) const
{
    const std::string kernelStr = kernelCode.str();
    const size_t parts = ceilDivide(kernelStr.length(), partLength);
    for(size_t i = 0; i < parts; i++) {
        os << "R\"(" << kernelStr.substr(i * partLength, partLength) << ")\"" << std::endl;
    }
}
//--------------------------------------------------------------------------
const PresynapticUpdateStrategy::Base* Backend::getPresynapticUpdateStrategy(const SynapseGroupInternal& sg)
{
    // Loop through presynaptic update strategies until we find one that is compatible with this synapse group
    // **NOTE** this is done backwards so that user-registered strategies get first priority
    for (auto s = s_PresynapticUpdateStrategies.rbegin(); s != s_PresynapticUpdateStrategies.rend(); ++s) {
        if ((*s)->isCompatible(sg)) {
            return *s;
        }
    }

    throw std::runtime_error("Unable to find a suitable presynaptic update strategy for synapse group '" + sg.getName() + "'");
    return nullptr;
}
} // namespace OpenCL
} // namespace CodeGenerator