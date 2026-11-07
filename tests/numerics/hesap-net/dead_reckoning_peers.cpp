// The peer of dead_reckoning_peers.hpp: KDIS's own DeadReckoningCalculator and the data types it uses, compiled from
// the pinned commit (tests/numerics/hesap-net/CMakeLists.txt) with DIS 7's fields.

#include "dead_reckoning_peers.hpp"

#include "KDIS/DataTypes/EulerAngles.hpp"
#include "KDIS/DataTypes/Vector.hpp"
#include "KDIS/DataTypes/WorldCoordinates.hpp"
#include "KDIS/Extras/DeadReckoningCalculator.hpp"

namespace dead_reckoning_peers
{

using KDIS::DATA_TYPE::EulerAngles;
using KDIS::DATA_TYPE::Vector;
using KDIS::DATA_TYPE::WorldCoordinates;
using KDIS::DATA_TYPE::ENUMS::DeadReckoningAlgorithm;
using KDIS::UTILS::DeadReckoningCalculator;

struct KdisFleet::Calculators
{
    std::unique_ptr<DeadReckoningCalculator[]> entities;
};

KdisFleet::KdisFleet(std::size_t count) : m_calculators(std::make_unique<Calculators>())
{
    m_calculators->entities = std::make_unique<DeadReckoningCalculator[]>(count);
}

KdisFleet::~KdisFleet() = default;

void KdisFleet::reset(std::size_t entity, const Update& update)
{
    EulerAngles orientation;
    orientation.SetPsiInRadians(update.psi);
    orientation.SetThetaInRadians(update.theta);
    orientation.SetPhiInRadians(update.phi);
    m_calculators->entities[entity].Reset(
        Vector(update.velocity[0], update.velocity[1], update.velocity[2]),
        Vector(update.acceleration[0], update.acceleration[1], update.acceleration[2]),
        Vector(update.rate[0], update.rate[1], update.rate[2]),
        WorldCoordinates(update.position[0], update.position[1], update.position[2]), orientation,
        Vector(0.0F, 0.0F, 0.0F), static_cast<DeadReckoningAlgorithm>(update.algorithm));
}

void KdisFleet::run(const float* dt, std::size_t count, double* x, double* y, double* z)
{
    WorldCoordinates position;
    EulerAngles orientation;
    for (std::size_t i = 0U; i < count; ++i)
    {
        m_calculators->entities[i].RunAlgorithm(dt[i], position, orientation);
        x[i] = position.GetX();
        y[i] = position.GetY();
        z[i] = position.GetZ();
    }
}

} // namespace dead_reckoning_peers
