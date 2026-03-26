#include "benchmark_experiment.h"
#include "binding.h"

int main(int argc, char* argv[]) {
  return benchmark::BenchmarkExperiment(experiment::CreateBinding("si-eager"))
      .Run(argc, argv);
}
