#include "benchmark_experiment.h"
#include "binding.h"

int main(int argc, char* argv[]) {
  return benchmark::BenchmarkExperiment(experiment::CreateBinding("no-index"))
      .Run(argc, argv);
}
