#include <crd/app/layer_stack.hpp>

namespace crd::app
{
namespace
{
[[nodiscard]] crd::usize find_index(const crd::containers::Array<Layer*>& layers, Layer* needle)
{
    for (crd::usize i = 0; i < layers.size(); ++i)
    {
        if (layers[i] == needle)
        {
            return i;
        }
    }
    return static_cast<crd::usize>(-1);
}
} // namespace

void LayerStack::push_layer(Layer* layer)
{
    m_layers.insert(m_overlay_insert_index, layer);
    ++m_overlay_insert_index;
}

void LayerStack::push_overlay(Layer* overlay)
{
    m_layers.push_back(overlay);
}

void LayerStack::pop_layer(Layer* layer)
{
    const crd::usize index = find_index(m_layers, layer);
    if (index == static_cast<crd::usize>(-1) || index >= m_overlay_insert_index)
    {
        return;
    }
    m_layers.erase(index);
    --m_overlay_insert_index;
}

void LayerStack::pop_overlay(Layer* overlay)
{
    const crd::usize index = find_index(m_layers, overlay);
    if (index == static_cast<crd::usize>(-1) || index < m_overlay_insert_index)
    {
        return;
    }
    m_layers.erase(index);
}
} // namespace crd::app
