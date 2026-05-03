#include "World.hpp"

#include <algorithm>
#include <cmath>

namespace arena
{
  World::World()
      : obstacles_{
            {420.0, 260.0, 240.0, 80.0},
            {820.0, 640.0, 120.0, 260.0},
            {1280.0, 220.0, 320.0, 90.0},
            {1450.0, 780.0, 180.0, 180.0},
            {250.0, 860.0, 280.0, 100.0}}
  {
  }

  std::pair<double, double> World::randomSpawn(std::mt19937 &rng) const
  {
    std::uniform_real_distribution<double> xdist(playerRadius + 20.0, width_ - playerRadius - 20.0);
    std::uniform_real_distribution<double> ydist(playerRadius + 20.0, height_ - playerRadius - 20.0);

    for (int i = 0; i < 80; ++i)
    {
      double x = xdist(rng);
      double y = ydist(rng);
      if (!collides(x, y))
      {
        return {x, y};
      }
    }

    return {width_ * 0.5, height_ * 0.5};
  }

  bool World::collides(double x, double y, double radius) const
  {
    if (x < radius || y < radius || x > width_ - radius || y > height_ - radius)
    {
      return true;
    }

    for (const auto &o : obstacles_)
    {
      const double nearestX = std::clamp(x, o.x, o.x + o.w);
      const double nearestY = std::clamp(y, o.y, o.y + o.h);
      const double dx = x - nearestX;
      const double dy = y - nearestY;
      if ((dx * dx + dy * dy) < radius * radius)
      {
        return true;
      }
    }

    return false;
  }

  void World::clampToBounds(double &x, double &y, double radius) const
  {
    x = std::clamp(x, radius, width_ - radius);
    y = std::clamp(y, radius, height_ - radius);
  }

  nlohmann::json World::toJson() const
  {
    nlohmann::json obstacles = nlohmann::json::array();
    for (const auto &o : obstacles_)
    {
      obstacles.push_back({{"x", o.x}, {"y", o.y}, {"w", o.w}, {"h", o.h}});
    }

    return {
        {"width", width_},
        {"height", height_},
        {"obstacles", obstacles}};
  }
}
