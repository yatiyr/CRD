#pragma once

#include <crd/app/layer.hpp>
#include <crd/containers/array.hpp>

#include <iterator>

namespace crd::app
{
class LayerStack
{
public:
    using iterator = Layer**;
    using const_iterator = Layer* const*;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    void push_layer(Layer* layer);
    void push_overlay(Layer* overlay);
    void pop_layer(Layer* layer);
    void pop_overlay(Layer* overlay);

    [[nodiscard]] iterator begin() noexcept { return m_layers.begin(); }
    [[nodiscard]] iterator end() noexcept { return m_layers.end(); }
    [[nodiscard]] const_iterator begin() const noexcept { return m_layers.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return m_layers.end(); }
    [[nodiscard]] reverse_iterator rbegin() noexcept { return reverse_iterator(m_layers.end()); }
    [[nodiscard]] reverse_iterator rend() noexcept { return reverse_iterator(m_layers.begin()); }
    [[nodiscard]] const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(m_layers.end()); }
    [[nodiscard]] const_reverse_iterator rend() const noexcept { return const_reverse_iterator(m_layers.begin()); }

    [[nodiscard]] crd::usize size() const noexcept { return m_layers.size(); }

private:
    crd::containers::Array<Layer*> m_layers{};
    crd::usize m_overlay_insert_index = 0;
};
} // namespace crd::app
