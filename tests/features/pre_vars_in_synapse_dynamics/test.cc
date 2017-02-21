// Google test includes
#include "gtest/gtest.h"

// Autogenerated simulation code includess
#include "pre_vars_in_synapse_dynamics_CODE/definitions.h"

// **NOTE** base-class for simulation tests must be
// included after auto-generated globals are includes
#include "../../utils/simulation_test_vars.h"
#include "../../utils/simulation_neuron_policy_pre_var.h"
#include "../../utils/simulation_synapse_policy_dense.h"

// Combine neuron and synapse policies together to build variable-testing fixture
typedef SimulationTestVars<SimulationNeuronPolicyPreVar, SimulationSynapsePolicyDense> SimulationTestPreVars;

TEST_P(SimulationTestPreVars, AcceptableError)
{
  float err = Simulate(
    [](unsigned int d, unsigned int j, unsigned int k, float t, float &newX)
    {
        if (t > 0.0001+(d+1)*DT)
        {
            newX = t-DT-(d+1)*DT+10*j;
            return true;
        }
        else
        {
          return false;
        }
    });

  // Check total error is less than some tolerance
  EXPECT_LT(err, 5e-2);
}

#ifndef CPU_ONLY
auto simulatorBackends = ::testing::Values(true, false);
#else
auto simulatorBackends = ::testing::Values(false);
#endif

INSTANTIATE_TEST_CASE_P(SynapseDynamics,
                        SimulationTestPreVars,
                        simulatorBackends);