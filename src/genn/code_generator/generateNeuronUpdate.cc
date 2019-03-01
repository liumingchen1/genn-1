#include "code_generator/generateNeuronUpdate.h"

// Standard C++ includes
#include <iostream>
#include <string>

// PLOG includes
#include <plog/Log.h>

// GeNN includes
#include "models.h"
#include "modelSpec.h"

// GeNN code generator includes
#include "code_generator/codeGenUtils.h"
#include "code_generator/codeStream.h"
#include "code_generator/substitutions.h"
#include "code_generator/backendBase.h"

//--------------------------------------------------------------------------
// Anonymous namespace
//--------------------------------------------------------------------------
namespace
{
void applyNeuronModelSubstitutions(std::string &code, const NeuronGroup &ng,
                                   const std::string &varSuffix = "", const std::string &varExt = "")
{
    using namespace CodeGenerator;

    const NeuronModels::Base *nm = ng.getNeuronModel();

    // Create iteration context to iterate over the variables; derived and extra global parameters
    VarNameIterCtx nmVars(nm->getVars());
    DerivedParamNameIterCtx nmDerivedParams(nm->getDerivedParams());
    ExtraGlobalParamNameIterCtx nmExtraGlobalParams(nm->getExtraGlobalParams());

    name_substitutions(code, "l", nmVars.nameBegin, nmVars.nameEnd, varSuffix, varExt);
    value_substitutions(code, nm->getParamNames(), ng.getParams());
    value_substitutions(code, nmDerivedParams.nameBegin, nmDerivedParams.nameEnd, ng.getDerivedParams());
    name_substitutions(code, "", nmExtraGlobalParams.nameBegin, nmExtraGlobalParams.nameEnd, ng.getName());
}
//--------------------------------------------------------------------------
void applyPostsynapticModelSubstitutions(std::string &code, const SynapseGroup *sg)
{
    using namespace CodeGenerator;

    const auto *psm = sg->getPSModel();

    // Create iterators to iterate over the names of the postsynaptic model's initial values
    VarNameIterCtx psmVars(psm->getVars());
    DerivedParamNameIterCtx psmDerivedParams(psm->getDerivedParams());
    ExtraGlobalParamNameIterCtx psmExtraGlobalParams(psm->getExtraGlobalParams());

    if (sg->getMatrixType() & SynapseMatrixWeight::INDIVIDUAL_PSM) {
        name_substitutions(code, "lps", psmVars.nameBegin, psmVars.nameEnd, sg->getName());
    }
    else {
        value_substitutions(code, psmVars.nameBegin, psmVars.nameEnd, sg->getPSConstInitVals());
    }
    value_substitutions(code, psm->getParamNames(), sg->getPSParams());

    // Create iterators to iterate over the names of the postsynaptic model's derived parameters
    value_substitutions(code, psmDerivedParams.nameBegin, psmDerivedParams.nameEnd, sg->getPSDerivedParams());
    name_substitutions(code, "", psmExtraGlobalParams.nameBegin, psmExtraGlobalParams.nameEnd, sg->getName());
}
}   // Anonymous namespace

//--------------------------------------------------------------------------
// CodeGenerator
//--------------------------------------------------------------------------
void CodeGenerator::generateNeuronUpdate(CodeStream &os, const NNmodel &model, const BackendBase &backend)
{
    os << "#include \"definitionsInternal.h\"" << std::endl;

    // Neuron update kernel
    backend.genNeuronUpdate(os, model,
        [&backend, &model](CodeStream &os, const NeuronGroup &ng, Substitutions &popSubs)
        {
            const NeuronModels::Base *nm = ng.getNeuronModel();

            // Generate code to copy neuron state into local variable
            for(const auto &v : nm->getVars()) {
                os << v.second << " l" << v.first << " = ";
                os << backend.getVarPrefix() << v.first << ng.getName() << "[";
                if (ng.isVarQueueRequired(v.first) && ng.isDelayRequired()) {
                    os << "readDelayOffset + ";
                }
                os << popSubs.getVarSubstitution("id") << "];" << std::endl;
            }
    
            // Also read spike time into local variable
            if(ng.isSpikeTimeRequired()) {
                os << model.getTimePrecision() << " lsT = " << backend.getVarPrefix() << "sT" << ng.getName() << "[";
                if (ng.isDelayRequired()) {
                    os << "readDelayOffset + ";
                }
                os << popSubs.getVarSubstitution("id") << "];" << std::endl;
            }
            os << std::endl;

            if (!ng.getMergedInSyn().empty() || (nm->getSimCode().find("Isyn") != std::string::npos)) {
                os << model.getPrecision() << " Isyn = 0;" << std::endl;
            }

            popSubs.addVarSubstitution("Isyn", "Isyn");
            popSubs.addVarSubstitution("sT", "lsT");

            // Initialise any additional input variables supported by neuron model
            for (const auto &a : nm->getAdditionalInputVars()) {
                os << a.second.first << " " << a.first << " = " << a.second.second << ";" << std::endl;
            }

            for (const auto &m : ng.getMergedInSyn()) {
                const auto *sg = m.first;
                const auto *psm = sg->getPSModel();

                os << "// pull inSyn values in a coalesced access" << std::endl;
                os << model.getPrecision() << " linSyn" << sg->getPSModelTargetName() << " = " << backend.getVarPrefix() << "inSyn" << sg->getPSModelTargetName() << "[" << popSubs.getVarSubstitution("id") << "];" << std::endl;

                // If dendritic delay is required
                if (sg->isDendriticDelayRequired()) {
                    // Get reference to dendritic delay buffer input for this timestep
                    os << model.getPrecision() << " &denDelayFront" << sg->getPSModelTargetName() << " = ";
                    os << backend.getVarPrefix() << "denDelay" + sg->getPSModelTargetName() + "[" + sg->getDendriticDelayOffset(backend.getVarPrefix()) + popSubs.getVarSubstitution("id") + "];" << std::endl;

                    // Add delayed input from buffer into inSyn
                    os << "linSyn" + sg->getPSModelTargetName() + " += denDelayFront" << sg->getPSModelTargetName() << ";" << std::endl;

                    // Zero delay buffer slot
                    os << "denDelayFront" << sg->getPSModelTargetName() << " = " << model.scalarExpr(0.0) << ";" << std::endl;
                }

                // If synapse group has individual postsynaptic variables, also pull these in a coalesced access
                if (sg->getMatrixType() & SynapseMatrixWeight::INDIVIDUAL_PSM) {
                    // **TODO** base behaviour from Models::Base
                    for (const auto &v : psm->getVars()) {
                        os << v.second << " lps" << v.first << sg->getPSModelTargetName();
                        os << " = " << backend.getVarPrefix() << v.first << sg->getPSModelTargetName() << "[" << popSubs.getVarSubstitution("id") << "];" << std::endl;
                    }
                }

                Substitutions inSynSubs(&popSubs);
                inSynSubs.addVarSubstitution("inSyn", "linSyn" + sg->getPSModelTargetName());

                // Apply substitutions to current converter code
                std::string psCode = psm->getApplyInputCode();
                applyNeuronModelSubstitutions(psCode, ng);
                applyPostsynapticModelSubstitutions(psCode, sg);
                inSynSubs.apply(psCode);
                psCode = ensureFtype(psCode, model.getPrecision());
                checkUnreplacedVariables(psCode, sg->getPSModelTargetName() + " : postSyntoCurrent");

                if (!psm->getSupportCode().empty()) {
                    os << CodeStream::OB(29) << " using namespace " << sg->getPSModelTargetName() << "_postsyn;" << std::endl;
                }
                os << psCode << std::endl;
                if (!psm->getSupportCode().empty()) {
                    os << CodeStream::CB(29) << " // namespace bracket closed" << std::endl;
                }
            }

            // Loop through all of neuron group's current sources
            for (const auto *cs : ng.getCurrentSources())
            {
                os << "// current source " << cs->getName() << std::endl;
                CodeStream::Scope b(os);

                const auto* csm = cs->getCurrentSourceModel();

                // Read current source variables into registers
                for(const auto &v : csm->getVars()) {
                    os <<  v.second << " lcs" << v.first << " = " << backend.getVarPrefix() << v.first << cs->getName() << "[" << popSubs.getVarSubstitution("id") << "];" << std::endl;
                }

                Substitutions currSourceSubs(&popSubs);
                currSourceSubs.addFuncSubstitution("injectCurrent", 1, "Isyn += $(0)");

                std::string iCode = csm->getInjectionCode();

                // Create iteration context to iterate over the variables; derived and extra global parameters
                VarNameIterCtx csVars(csm->getVars());
                DerivedParamNameIterCtx csDerivedParams(csm->getDerivedParams());
                ExtraGlobalParamNameIterCtx csExtraGlobalParams(csm->getExtraGlobalParams());

                name_substitutions(iCode, "lcs", csVars.nameBegin, csVars.nameEnd);
                value_substitutions(iCode, csm->getParamNames(), cs->getParams());
                value_substitutions(iCode, csDerivedParams.nameBegin, csDerivedParams.nameEnd, cs->getDerivedParams());
                name_substitutions(iCode, "", csExtraGlobalParams.nameBegin, csExtraGlobalParams.nameEnd, cs->getName());

                currSourceSubs.apply(iCode);
                iCode = ensureFtype(iCode, model.getPrecision());
                checkUnreplacedVariables(iCode, cs->getName() + " : current source injectionCode");
                os << iCode << std::endl;

                // Write updated variables back to global memory
                for(const auto &v : csm->getVars()) {
                    os << backend.getVarPrefix() << v.first << cs->getName() << "[" << popSubs.getVarSubstitution("id") << "] = lcs" << v.first << ";" << std::endl;
                }
            }

            if (!nm->getSupportCode().empty()) {
                os << " using namespace " << ng.getName() << "_neuron;" << std::endl;
            }

            std::string thCode = nm->getThresholdConditionCode();
            if (thCode.empty()) { // no condition provided
                LOGW << "No thresholdConditionCode for neuron type " << typeid(*nm).name() << " used for population \"" << ng.getName() << "\" was provided. There will be no spikes detected in this population!";
            }
            else {
                os << "// test whether spike condition was fulfilled previously" << std::endl;

                applyNeuronModelSubstitutions(thCode, ng);
                popSubs.apply(thCode);
                thCode= ensureFtype(thCode, model.getPrecision());
                checkUnreplacedVariables(thCode, ng.getName() + " : thresholdConditionCode");

                if (nm->isAutoRefractoryRequired()) {
                    os << "const bool oldSpike= (" << thCode << ");" << std::endl;
                }
            }

            os << "// calculate membrane potential" << std::endl;
            std::string sCode = nm->getSimCode();
            popSubs.apply(sCode);

            applyNeuronModelSubstitutions(sCode, ng);

            sCode = ensureFtype(sCode, model.getPrecision());
            checkUnreplacedVariables(sCode, ng.getName() + " : neuron simCode");

            os << sCode << std::endl;

            // look for spike type events first.
            if (ng.isSpikeEventRequired()) {
                // Create local variable
                os << "bool spikeLikeEvent = false;" << std::endl;

                // Loop through outgoing synapse populations that will contribute to event condition code
                for(const auto &spkEventCond : ng.getSpikeEventCondition()) {
                    // Replace of parameters, derived parameters and extraglobalsynapse parameters
                    std::string eCode = spkEventCond.first;
                    applyNeuronModelSubstitutions(eCode, ng, "", "_pre");
                    popSubs.apply(eCode);
                    eCode = ensureFtype(eCode, model.getPrecision());
                    checkUnreplacedVariables(eCode, ng.getName() + " : neuronSpkEvntCondition");

                    // Open scope for spike-like event test
                    os << CodeStream::OB(31);

                    // Use synapse population support code namespace if required
                    if (!spkEventCond.second.empty()) {
                        os << " using namespace " << spkEventCond.second << ";" << std::endl;
                    }

                    // Combine this event threshold test with
                    os << "spikeLikeEvent |= (" << eCode << ");" << std::endl;

                    // Close scope for spike-like event test
                    os << CodeStream::CB(31);
                }

                os << "// register a spike-like event" << std::endl;
                os << "if (spikeLikeEvent)";
                {
                    CodeStream::Scope b(os);
                    backend.genEmitSpikeLikeEvent(os, model, ng, popSubs);
                }
            }

            // test for true spikes if condition is provided
            if (!thCode.empty()) {
                os << "// test for and register a true spike" << std::endl;
                if (nm->isAutoRefractoryRequired()) {
                    os << "if ((" << thCode << ") && !(oldSpike))";
                }
                else {
                    os << "if (" << thCode << ")";
                }
                {
                    CodeStream::Scope b(os);

                    backend.genEmitTrueSpike(os, model, ng, popSubs);

                    // add after-spike reset if provided
                    if (!nm->getResetCode().empty()) {
                        std::string rCode = nm->getResetCode();
                        applyNeuronModelSubstitutions(rCode, ng);
                        popSubs.apply(rCode);
                        rCode = ensureFtype(rCode, model.getPrecision());
                        checkUnreplacedVariables(rCode, ng.getName() + " : resetCode");

                        os << "// spike reset code" << std::endl;
                        os << rCode << std::endl;
                    }
                }
            }

            // store the defined parts of the neuron state into the global state variables dd_V etc
            for(const auto &v : nm->getVars()) {
                os << backend.getVarPrefix() << v.first << ng.getName() << "[";

                if (ng.isVarQueueRequired(v.first) && ng.isDelayRequired()) {
                    os << "writeDelayOffset + ";
                }
                os << popSubs.getVarSubstitution("id") << "] = l" << v.first << ";" << std::endl;
            }

            for (const auto &m : ng.getMergedInSyn()) {
                const auto *sg = m.first;
                const auto *psm = sg->getPSModel();

                Substitutions inSynSubs(&popSubs);
                inSynSubs.addVarSubstitution("inSyn", "linSyn" + sg->getPSModelTargetName());

                std::string pdCode = psm->getDecayCode();
                applyNeuronModelSubstitutions(pdCode, ng);
                applyPostsynapticModelSubstitutions(pdCode, sg);
                inSynSubs.apply(pdCode);
                pdCode = ensureFtype(pdCode, model.getPrecision());
                checkUnreplacedVariables(pdCode, sg->getPSModelTargetName() + " : postSynDecay");

                os << "// the post-synaptic dynamics" << std::endl;
                if (!psm->getSupportCode().empty()) {
                    os << CodeStream::OB(29) << " using namespace " << sg->getName() << "_postsyn;" << std::endl;
                }
                os << pdCode << std::endl;
                if (!psm->getSupportCode().empty()) {
                    os << CodeStream::CB(29) << " // namespace bracket closed" << std::endl;
                }

                os << backend.getVarPrefix() << "inSyn"  << sg->getPSModelTargetName() << "[" << inSynSubs.getVarSubstitution("id") << "] = linSyn" << sg->getPSModelTargetName() << ";" << std::endl;
                for (const auto &v : psm->getVars()) {
                    os << backend.getVarPrefix() << v.first << sg->getPSModelTargetName() << "[" << inSynSubs.getVarSubstitution("id") << "]" << " = lps" << v.first << sg->getPSModelTargetName() << ";" << std::endl;
                }
            }
        }
    );
}