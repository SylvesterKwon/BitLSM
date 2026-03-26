#include "benchmark_experiment.h"
#include "binding.h"

int main(int argc, char* argv[]) {
  return benchmark::BenchmarkExperiment(experiment::CreateBinding("bitlsm"))
      .Run(argc, argv);
}
