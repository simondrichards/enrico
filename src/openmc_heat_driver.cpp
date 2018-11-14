#include "stream/openmc_heat_driver.h"

#include "stream/message_passing.h"

#include <gsl/gsl>
#include "openmc/constants.h"
#include "xtensor/xstrided_view.hpp"

#include <cmath>
#include <unordered_map>

namespace stream {

OpenmcHeatDriver::OpenmcHeatDriver(MPI_Comm comm, pugi::xml_node node)
  : comm_{comm}
{
  // Get coupling parameters
  power_ = node.child("power").text().as_double();
  max_timesteps_ = node.child("max_timesteps").text().as_int();
  max_picard_iter_ = node.child("max_picard_iter").text().as_int();

  // Initialize OpenMC and surrogate heat drivers
  openmc_driver_ = std::make_unique<OpenmcDriver>(comm);
  pugi::xml_node surr_node = node.child("heat_surrogate");
  heat_driver_ = std::make_unique<SurrogateHeatDriver>(comm, surr_node);

  // Create mappings for fuel pins and setup tallies for OpenMC
  init_mappings();
  init_tallies();

}

void OpenmcHeatDriver::init_mappings()
{
  using openmc::PI;

  int nrings = heat_driver_->n_fuel_rings_;
  const auto& r_fuel = heat_driver_->r_grid_fuel_;
  const auto& r_clad = heat_driver_->r_grid_clad_;
  const auto& centers = heat_driver_->pin_centers_;
  const auto& z = heat_driver_->z_;
  auto nz = heat_driver_->n_axial_;
  auto npins = centers.shape()[0];

  std::unordered_map<CellInstance, int> tracked;
  int ring_index = 0;
  // TODO: Don't hardcode number of azimuthal segments
  int n_azimuthal = 4;
  for (int i = 0; i < npins; ++i) {
    for (int j = 0; j < nz; ++j) {
      // Get average z value
      double zavg = 0.5*(z(j) + z(j + 1));

      // Loop over radial rings
      for (int k = 0; k < heat_driver_->n_rings(); ++k) {
        double ravg;
        if (k < heat_driver_->n_fuel_rings_) {
          ravg = 0.5*(r_fuel(k) + r_fuel(k + 1));
        } else {
          int m = k - heat_driver_->n_fuel_rings_;
          ravg = 0.5*(r_clad(m) + r_clad(m + 1));
        }

        for (int m = 0; m < n_azimuthal; ++m) {
          double theta = 2.0*m*PI/n_azimuthal + 0.01;
          double x = ravg * std::cos(theta);
          double y = ravg * std::sin(theta);

          // Determine cell instance corresponding to given pin location
          Position r {x, y, zavg};
          CellInstance c {r};
          if (tracked.find(c) == tracked.end()) {
            openmc_driver_->cells_.push_back(c);
            tracked[c] = openmc_driver_->cells_.size() - 1;
          }

          // Map OpenMC material to ring and vice versa
          int32_t array_index = tracked[c];
          cell_inst_to_ring_[array_index].push_back(ring_index);
          ring_to_cell_inst_[ring_index].push_back(array_index);
        }

        ++ring_index;
      }
    }
  }
}

void OpenmcHeatDriver::init_tallies()
{
  if (openmc_driver_->active()) {
    // Build vector of material indices
    std::vector<int32_t> mats;
    for (const auto& c : openmc_driver_->cells_) {
      mats.push_back(c.material_index_ - 1);
    }
    openmc_driver_->create_tallies(mats);
  }
}

void OpenmcHeatDriver::solve_step()
{
  for (int i_timestep = 0; i_timestep < max_timesteps_; ++i_timestep) {
    for (int i_picard = 0; i_picard < max_picard_iter_; ++i_picard) {

      // Solve neutron transport
      if (openmc_driver_->active()) {
        openmc_driver_->init_step();
        int i = i_timestep*max_timesteps_ + i_picard;
        openmc_driver_->solve_step(i);
        openmc_driver_->finalize_step();
      }
      comm_.Barrier();

      // Update heat source
      update_heat_source();

      // Solve heat equation
      if (heat_driver_->active()) {
        heat_driver_->solve_step();
      }
      comm_.Barrier();

      // Update temperature in OpenMC
      update_temperature();
    }
  }
}

void OpenmcHeatDriver::update_heat_source()
{
  // zero out heat source
  for (auto& val : heat_driver_->source_) {
    val = 0.0;
  }

  // Determine heat source based on OpenMC tally results
  auto Q = openmc_driver_->heat_source(power_);

  int ring_index = 0;
  for (int i = 0; i < heat_driver_->n_pins_; ++i) {
    for (int j = 0; j < heat_driver_->n_axial_; ++j) {
      // Loop over radial rings
      for (int k = 0; k < heat_driver_->n_rings(); ++k) {
        // Only update heat source in fuel
        if (k < heat_driver_->n_fuel_rings_) {
          // Get average Q value across each azimuthal segment
          const auto& cell_instances = ring_to_cell_inst_[ring_index];
          double q_avg = 0.0;
          for (auto idx : cell_instances) {
            q_avg += Q(idx);
          }
          q_avg /= cell_instances.size();

          // Set Q in appropriate (pin, axial, ring)
          heat_driver_->source_.at(i, j, k) = q_avg;
        }
        ++ring_index;
      }
    }
  }
}

void OpenmcHeatDriver::update_temperature()
{
  int nrings = heat_driver_->n_fuel_rings_;
  const auto& r_fuel = heat_driver_->r_grid_fuel_;
  const auto& r_clad = heat_driver_->r_grid_clad_;

  // The temperatures array normally has three dimensions, but the mapping we
  // have gives a flattened index, so we need to get a flattened view of the
  // temperature array
  auto temperature = xt::flatten(heat_driver_->temperature_);

  // For each OpenMC material, volume average temperatures and set
  for (int i = 0; i < openmc_driver_->cells_.size(); ++i) {
    // Get cell instance
    const auto& c = openmc_driver_->cells_[i];

    // Get rings corresponding to this cell instance
    const auto& rings = cell_inst_to_ring_[i];

    // Get volume-average temperature for this material
    double average_temp = 0.0;
    double total_vol = 0.0;
    for (int ring_index : rings) {
      // Use difference in r**2 as a proxy for volume. This is only used for
      // averaging, so the absolute value doesn't matter
      int i = ring_index % heat_driver_->n_rings();
      double vol;
      if (i < heat_driver_->n_fuel_rings_) {
        vol = r_fuel(i+1)*r_fuel(i+1) - r_fuel(i)*r_fuel(i);
      } else {
        int j = i - heat_driver_->n_fuel_rings_;
        vol = r_clad(j+1)*r_clad(j+1) - r_clad(j)*r_clad(j);
      }

      average_temp += temperature(ring_index) * vol;
      total_vol += vol;
    }
    average_temp /= total_vol;

    // Set temperature for cell instance
    c.set_temperature(average_temp);
  }
}

} // namespace stream
