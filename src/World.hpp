#pragma once

#include <random>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace arena
{
  struct Obstacle
  {
    double x{0.0};
    double y{0.0};
    double w{0.0};
    double h{0.0};
  };

  class World
  {
  public:
    static constexpr double playerRadius = 18.0;

    World();

    [[nodiscard]] double width() const noexcept { return width_; }
    [[nodiscard]] double height() const noexcept { return height_; }
    [[nodiscard]] const std::vector<Obstacle> &obstacles() const noexcept { return obstacles_; }

    [[nodiscard]] std::pair<double, double> randomSpawn(std::mt19937 &rng) const;
    [[nodiscard]] bool collides(double x, double y, double radius = playerRadius) const;
    void clampToBounds(double &x, double &y, double radius = playerRadius) const;
    [[nodiscard]] nlohmann::json toJson() const;

  private:
    double width_{2000.0};
    double height_{1200.0};
    std::vector<Obstacle> obstacles_;
  };
}
