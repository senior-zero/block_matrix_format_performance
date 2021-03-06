//
// Created by egi on 12/1/19.
//

#ifndef BLOCK_MATRIX_FORMAT_PERFORMANCE_GOLDEN_GATE_BRIDGE_H
#define BLOCK_MATRIX_FORMAT_PERFORMANCE_GOLDEN_GATE_BRIDGE_H

#include "matrix_converters.h"
#include "mmio.h"

#include <fstream>
#include <memory>
#include <cmath>
#include <array>

/// C = A * B
template <class data_type, class index_type>
void matrix_mult_matrix (const data_type *a, const data_type *b, data_type *c, const index_type n)
{
  for (index_type i = 0; i < n; i++)
    {
      for (index_type j = 0; j < n; j++)
        {
          data_type sum = 0.0;
          for (index_type k = 0; k < n; k++)
            sum += a[i * n + k] * b[k * n + j];
          c[i * n + j] = sum;
        }
    }
}

/// C = A^T * B
template <class data_type, class index_type>
void matrix_transponse_and_mult (const data_type *a, const data_type *b, data_type *c, const index_type n)
{
  for (index_type i = 0; i < n; i++)
    {
      for (index_type j = 0; j < n; j++)
        {
          data_type sum = 0.0;
          for (index_type k = 0; k < n; k++)
            sum += a[k * n + i] * b[k * n + j];
          c[i * n + j] = sum;
        }
    }
}

template <typename data_type, typename index_type, bool use_frames>
class golden_gate_bridge_2d
{
  /**
   * Segment:
   *
   *       0    1    2
   *       +----+----+
   *       |   /|\   |
   *       |  / | \  |
   *       | /  |  \ |
   *       |/   |   \|
   *       +----+----+
   *       3    4    5
   */

  constexpr static const index_type stiffness_matrix_block_size = use_frames ? 6 : 4; ///< Local stiffness matrix size
  const data_type side_length = 345.0; ///< Size from bridge tower to bank in meters
  const data_type main_part_length = 1280.0; ///< Size from tower to tower in meters
  const data_type tower_height = 230.0; ///< Height of tower in meters (from water level)
  const data_type bridge_height = 78.0;
  const data_type section_height = 7.62; ///< In meters

  const data_type segment_length = 10; ///< Size of segment in meters
  const index_type segments_count {};

  index_type elements_count {};

  index_type first_available_node_id {};
  index_type first_available_element_id {};

  index_type left_tower_bottom {};
  index_type left_tower_top {};

  index_type right_tower_bottom {};
  index_type right_tower_top {};

  index_type last_road_node {};

  const data_type steel_e = 2e+11;
  const data_type rope_e = steel_e;
  const data_type tower_e = steel_e;
  const data_type segment_e = steel_e;
  const data_type spin_e = steel_e;

  const data_type spin_a = 0.36; /// meters
  const data_type rope_a = 0.06; /// meters
  const data_type tower_a = 0.62; /// meters
  const data_type segment_a = 0.45; /// meters

public:

  template <typename function_type>
  explicit golden_gate_bridge_2d (
    const function_type &load,
    data_type main_part_length_arg,
    data_type side_length_arg,
    data_type tower_height_arg = 230.0,
    data_type segment_length_arg = 7.62)
    : main_part_length (main_part_length_arg)
    , side_length (side_length_arg)
    , tower_height (tower_height_arg)
    , segment_length (segment_length_arg)
    , segments_count (calculate_segments_count())
    , elements_count (calculate_elements_count())
    , nodes_count (calculate_nodes_count())
    , nodes_xs (new data_type[nodes_count])
    , nodes_ys (new data_type[nodes_count])
    , nodes_x_bc (new int[nodes_count])
    , nodes_y_bc (new int[nodes_count])
    , elements_areas (new data_type[elements_count])
    , elements_e (new data_type[elements_count])
    , dxs (new data_type[elements_count])
    , dys (new data_type[elements_count])
    , lens (new data_type[elements_count])
    , stiffness_matrix (new data_type[stiffness_matrix_block_size * stiffness_matrix_block_size * elements_count])
    , forces_rhs (new data_type[2 * nodes_count])
    , elements_starts (new index_type[elements_count])
    , elements_ends (new index_type[elements_count])
  {
    std::fill_n (elements_starts.get (), elements_count, 0);
    std::fill_n (elements_ends.get (), elements_count, 0);
    std::fill_n (nodes_xs.get (), nodes_count, 0.0);
    std::fill_n (nodes_ys.get (), nodes_count, 0.0);
    std::fill_n (nodes_x_bc.get (), nodes_count, 0);
    std::fill_n (nodes_y_bc.get (), nodes_count, 0);
    std::fill_n (forces_rhs.get (), 2 * nodes_count, 0.0);

    fill_road_part ();
    fill_tower_part ();
    fill_side_spin_and_ropes ();
    fill_main_spin_and_ropes ();

    if (false)
      {
        nodes_xs[0] = 0;
        nodes_xs[1] = 1;
        nodes_ys[0] = 0;
        nodes_ys[1] = 0;

        set_element (0, 0, 1, 1.0, 100);

        first_available_node_id = 2;
        first_available_element_id = 1;

        nodes_x_bc[0] = 1;
        nodes_y_bc[0] = 1;
        nodes_x_bc[1] = 0;
        nodes_y_bc[1] = 0;

        forces_rhs[0] = 0;
        forces_rhs[1] = 0;
        forces_rhs[2] = 1000;
        forces_rhs[3] = 0;
        // nodes_y_bc[1] = 1;

        // nodes_x_bc[2] = 1;
        // nodes_y_bc[3] = 1;
      }

    elements_count = first_available_element_id;
    nodes_count = first_available_node_id;

    finalize_elements ();
    calculate_local_stiffness_matrices ();
    assemble_matrix ();

    const index_type bs = use_frames ? 3 : 2;

    for (index_type segment_id = 0; segment_id < segments_count; segment_id++)
      {
        const index_type n_1 = segment_id * 4 + 0;
        const index_type n_2 = segment_id * 4 + 1;

        std::tie (forces_rhs[n_1 * bs + 0], forces_rhs[n_1 * bs + 1]) = load (nodes_xs[n_1]);
        std::tie (forces_rhs[n_2 * bs + 0], forces_rhs[n_2 * bs + 1]) = load (nodes_xs[n_2]);
      }

    // export nodes
    {
      std::ofstream nodes ("nodes");
      nodes << nodes_count << "\n";

      for (index_type node = 0; node < nodes_count; node++)
        nodes << node << " " << nodes_xs[node] << " " << nodes_ys[node] << "\n";
      nodes << std::endl;
    }

    // export forces
    {
      std::ofstream forces ("forces");
      forces << nodes_count << "\n";

      for (index_type node = 0; node < nodes_count; node++)
        forces << node << " " << forces_rhs[node * 2 + 0] << " " << forces_rhs[node * 2 + 1] << "\n";
      forces << std::endl;
    }

    // export elements
    {
      std::ofstream elements ("elements");
      elements << elements_count << "\n";

      for (index_type element = 0; element < elements_count; element++)
        elements << element << " " << elements_starts[element]
                            << " " << elements_ends[element] << " "
                 << elements_areas[element]
                 << " " << elements_e[element] << "\n";
      elements << std::endl;

      index_type bc_count = 0;
      for (index_type node = 0; node < nodes_count; node++)
        {
          if (nodes_x_bc[node])
            bc_count++;
          if (nodes_y_bc[node])
            bc_count++;
        }

      elements << bc_count << "\n";

      for (index_type node = 0; node < nodes_count; node++)
        {
          if (nodes_x_bc[node])
            elements << node << " 0\n";
          if (nodes_y_bc[node])
            elements << node << " 1\n";
        }
    }
  }

  void write_vtk (const std::string &filename, const data_type *displacement = nullptr)
  {
    std::ofstream vtk (filename);

    vtk << "# vtk DataFile Version 3.0\n";
    vtk << "vtk output\n";
    vtk << "ASCII\n";
    vtk << "DATASET UNSTRUCTURED_GRID\n";

    vtk << "POINTS " << nodes_count << " double\n";

    const index_type bs = use_frames ? 3 : 2;

    if (displacement)
      {
        for (index_type node_id = 0; node_id < nodes_count; node_id++)
          vtk << nodes_xs[node_id] + displacement[node_id * bs + 0] << " "
              << nodes_ys[node_id] + displacement[node_id * bs + 1] << " 0\n";
      }
    else
      {
        for (index_type node_id = 0; node_id < nodes_count; node_id++)
          vtk << nodes_xs[node_id] << " "
              << nodes_ys[node_id] << " 0\n";
      }

    vtk << "CELLS " << elements_count << " " << 3 * elements_count << "\n";

    for (unsigned int element_id = 0; element_id < elements_count; element_id++)
      vtk << "2 " << elements_starts[element_id] << " " << elements_ends[element_id] << "\n";

    vtk << "CELL_TYPES " << elements_count << "\n";

    for (unsigned int element_id = 0; element_id < elements_count; element_id++)
      vtk << "3\n"; ///< VTK_LINE

    vtk << "POINT_DATA " << nodes_count << "\n";
    vtk << "SCALARS AvgDisplacement double 1\n";
    vtk << "LOOKUP_TABLE default\n";

    for (index_type node = 0; node < nodes_count; node++)
      vtk << (displacement ? (std::abs (displacement[node * bs + 0]) + std::abs (displacement[node * bs + 1])) / 2 : 0.0) << "\n";
    vtk << "\n";
  }

private:
  void set_element (index_type id, index_type begin, index_type end, data_type area, data_type e /* young's modulus */)
  {
    elements_starts[id] = std::min (begin, end);
    elements_ends[id] = std::max (begin, end);
    elements_areas[id] = area;
    elements_e[id] = e;
  }

  index_type calculate_segments_count ()
  {
    const data_type total_length = main_part_length + 2 * side_length;
    return total_length / segment_length + 1;
  }

  index_type calculate_elements_count ()
  {
    const index_type elements_count_in_road_part = segments_count * 8 + 4;
    const index_type towers_part = 2;
    const index_type main_spin_elements_count = segments_count;
    const index_type ropes_elements_count = segments_count;

    return elements_count_in_road_part + towers_part + ropes_elements_count + main_spin_elements_count;
  }

  index_type calculate_nodes_count ()
  {
    const index_type nodes_count_in_road_part = segments_count * 4 + 2;
    const index_type ropes_top_nodes_count = segments_count;
    const index_type main_spin_elements_count = segments_count;
    const index_type towers_part = 4;

    return nodes_count_in_road_part + towers_part + ropes_top_nodes_count + main_spin_elements_count;
  }

  void fill_road_part ()
  {
    const data_type dx = segment_length / 2;

    nodes_x_bc[0] = 1;
    nodes_y_bc[0] = 1;

    nodes_x_bc[2] = 1;
    nodes_y_bc[2] = 1;

    for (index_type segment_id = 0; segment_id < segments_count; segment_id++)
      {
        /// n_1 < n_2 < n_3 < n_4
        const index_type n_1 = segment_id * 4 + 0;
        const index_type n_2 = segment_id * 4 + 1;
        const index_type n_3 = segment_id * 4 + 2;
        const index_type n_4 = segment_id * 4 + 3;

        const index_type n_1_n = (segment_id + 1) * 4 + 0;
        const index_type n_3_n = segment_id == segments_count - 1 ? n_1_n + 1 : (segment_id + 1) * 4 + 2;

        /// Points 1 - 2
        nodes_xs[n_1] = segment_length * segment_id;
        nodes_xs[n_2] = segment_length * segment_id + dx;

        nodes_ys[n_1] = bridge_height;
        nodes_ys[n_2] = bridge_height;

        /// Points 4 - 5
        nodes_xs[n_3] = segment_length * segment_id;
        nodes_xs[n_4] = segment_length * segment_id + dx;

        nodes_ys[n_3] = bridge_height - section_height;
        nodes_ys[n_4] = bridge_height - section_height;

        const index_type e_1 = segment_id * 8 + 0;
        const index_type e_2 = segment_id * 8 + 1;
        const index_type e_3 = segment_id * 8 + 2;
        const index_type e_4 = segment_id * 8 + 3;
        const index_type e_5 = segment_id * 8 + 4;
        const index_type e_6 = segment_id * 8 + 5;
        const index_type e_7 = segment_id * 8 + 6;
        const index_type e_8 = segment_id * 8 + 7;

        set_element (e_1, n_1, n_3,   segment_a, segment_e);
        set_element (e_2, n_2, n_4,   segment_a, segment_e);
        set_element (e_3, n_1, n_2,   segment_a, segment_e);
        set_element (e_4, n_3, n_4,   segment_a, segment_e);
        set_element (e_5, n_3, n_2,   segment_a, segment_e);
        set_element (e_6, n_2, n_3_n, segment_a, segment_e);
        set_element (e_7, n_2, n_1_n, segment_a, segment_e);
        set_element (e_8, n_4, n_3_n, segment_a, segment_e);
      }

    const index_type n_1 = segments_count * 4 + 0;
    const index_type n_3 = segments_count * 4 + 1;

    nodes_xs[n_1] = segment_length * segments_count;
    nodes_xs[n_3] = segment_length * segments_count;

    nodes_ys[n_1] = bridge_height;
    nodes_ys[n_3] = bridge_height - section_height;

    nodes_x_bc[n_1] = 1;
    nodes_y_bc[n_1] = 1;

    nodes_x_bc[n_3] = 1;
    nodes_y_bc[n_3] = 1;

    const index_type e = segments_count * 8 + 0;
    set_element (e, n_1, n_3, segment_a, segment_e);

    last_road_node = n_1;

    first_available_node_id = n_3 + 1;
    first_available_element_id = e + 1;
  }

  void fill_tower_part ()
  {
    left_tower_bottom = first_available_node_id++;
    left_tower_top = first_available_node_id++;

    right_tower_bottom = first_available_node_id++;
    right_tower_top = first_available_node_id++;

    nodes_x_bc[left_tower_bottom] = 1;
    nodes_y_bc[left_tower_bottom] = 1;
    nodes_x_bc[right_tower_bottom] = 1;
    nodes_y_bc[right_tower_bottom] = 1;

    const index_type left_tower = first_available_element_id++;
    const index_type right_tower = first_available_element_id++;

    nodes_xs[left_tower_bottom]  = nodes_xs[left_tower_top]  = std::round (side_length / segment_length) * segment_length + segment_length / 2;
    nodes_xs[right_tower_bottom] = nodes_xs[right_tower_top] = std::round ((side_length + main_part_length) / segment_length) * segment_length + segment_length / 2;

    nodes_ys[right_tower_bottom] = nodes_ys[left_tower_bottom] = 0.0;
    nodes_ys[right_tower_top] = nodes_ys[left_tower_top] = tower_height;

    set_element (left_tower, left_tower_bottom, left_tower_top, tower_a, tower_e);
    set_element (right_tower, right_tower_bottom, right_tower_top, tower_a, tower_e);
  }

  void fill_side_spin_and_ropes ()
  {
    auto get_line_eq = [&] (const data_type y_1, const data_type y_2, const data_type x_1, const data_type x_2)
    {
      const data_type a = (y_1 - y_2);
      const data_type b = (x_2 - x_1);
      const data_type c = y_1 * x_2 - x_1 * y_2;

      return [=] (data_type x)
      {
        // Ax + By = C
        return (c - a * x) / b;
      };
    };

    /// Left
    auto get_y_left = get_line_eq (bridge_height, tower_height, 0, nodes_xs[left_tower_top]);
    const index_type first_left_side_spin_segment = first_available_element_id++;

    set_element (first_left_side_spin_segment, 0, first_available_node_id, rope_a, rope_e);

    for (index_type segment_id = 0; segment_id < (side_length - segment_length) / segment_length; segment_id++)
      {
        const index_type rope_bottom = segment_id * 4 + 1;
        const index_type rope_top = first_available_node_id++;
        const index_type rope = first_available_element_id++;

        nodes_xs[rope_top] = segment_length * segment_id + segment_length / 2;
        nodes_ys[rope_top] = get_y_left (nodes_xs[rope_top]);

        set_element (rope, rope_bottom, rope_top, rope_a, rope_e);

        if (segment_id > 0)
          {
            const index_type spin = first_available_element_id++;
            set_element (spin, rope_top - 1, rope_top, spin_a, spin_e);
          }
      }

    {
      const index_type rope_bottom = int (side_length / segment_length) * 4 + 1;
      set_element (first_available_element_id++, left_tower_top, rope_bottom, rope_a, rope_e);
    }

    const index_type last_left_side_spin_segment = first_available_element_id++;
    set_element (last_left_side_spin_segment, first_available_node_id - 1, left_tower_top, spin_a, spin_e);

    /// Left
    auto get_y_right = get_line_eq (tower_height, bridge_height, nodes_xs[right_tower_top], nodes_xs[last_road_node]);

    const index_type first_right_side_spin_segment = first_available_element_id++;
    set_element (first_right_side_spin_segment, right_tower_top, first_available_node_id, spin_a, spin_e);

    bool first_right_spine_segment = true;
    for (index_type segment_id = (side_length + main_part_length + segment_length) / segment_length;
         segment_id < (main_part_length + 2 * side_length - segment_length) / segment_length;
         segment_id++)
      {
        const index_type rope_bottom = segment_id * 4 + 1;
        const index_type rope_top = first_available_node_id++;
        const index_type rope = first_available_element_id++;

        nodes_xs[rope_top] = segment_length * segment_id + segment_length / 2;
        nodes_ys[rope_top] = get_y_right (nodes_xs[rope_top]);

        set_element (rope, rope_bottom, rope_top, rope_a, rope_e);

        if (first_right_spine_segment)
          {
            first_right_spine_segment = false;
          }
        else
          {
            const index_type spin = first_available_element_id++;
            set_element (spin, rope_top - 1, rope_top, spin_a, spin_e);
          }
      }

    {
      const index_type rope_bottom = int (side_length + main_part_length + segment_length/2) / segment_length * 4 + 1;
      set_element (first_available_element_id++, right_tower_top, rope_bottom, rope_a, rope_e);
    }

    const index_type last_right_side_spin_segment = first_available_element_id++;
    set_element (last_right_side_spin_segment, first_available_node_id - 1, segments_count * 4 + 0, spin_a, spin_e);
  }

  void fill_main_spin_and_ropes ()
  {
    const auto x_1 = nodes_xs[left_tower_top];
    const auto y_1 = nodes_ys[left_tower_top];

    const auto x_2 = (main_part_length + 2 * side_length) / 2;
    const auto y_2 = bridge_height + 1;

    const auto x_3 = nodes_xs[right_tower_top];
    const auto y_3 = nodes_ys[right_tower_top];

    const auto A_1 = x_2 * x_2 - (x_1 * x_1);
    const auto B_1 = x_2 - x_1;
    const auto D_1 = y_2 - y_1;
    const auto A_2 = x_3 * x_3 - x_2 * x_2;
    const auto B_2 = x_3 - x_2;
    const auto D_2 = y_3 - y_2;
    const auto B_mult = -(B_2 / B_1);
    const auto A_3 = B_mult * A_1 + A_2;
    const auto D_3 = B_mult * D_1 + D_2;
    const auto a = D_3 / A_3;
    const auto b = (D_1 - A_1 * a) / B_1;
    const auto c = y_1 - a * x_1 * x_1 - b * x_1;

    auto get_y = [=] (data_type x)
    {
      /// y = A*x^2 + B*x + C
      return a * x * x + b * x + c;
    };

    const index_type first_spin_segment = first_available_element_id++;
    set_element (first_spin_segment, left_tower_top, first_available_node_id, spin_a, spin_e);

    bool first_right_spine_segment = true;
    for (index_type segment_id = (side_length + segment_length) / segment_length;
         segment_id < (side_length + main_part_length - segment_length) / segment_length;
         segment_id++)
      {
        const index_type rope_bottom = segment_id * 4 + 1;
        const index_type rope_top = first_available_node_id++;
        const index_type rope = first_available_element_id++;

        nodes_xs[rope_top] = segment_length * segment_id + segment_length / 2;
        nodes_ys[rope_top] = get_y (nodes_xs[rope_top]);

        set_element (rope, rope_bottom, rope_top, rope_a, rope_e);

        if (first_right_spine_segment)
          {
            first_right_spine_segment = false;
          }
        else
          {
            const index_type spin = first_available_element_id++;
            set_element (spin, rope_top - 1, rope_top, spin_a, spin_e);
          }
      }

    const index_type last_spin_segment = first_available_element_id++;
    set_element (last_spin_segment, right_tower_top, first_available_node_id  - 1, spin_a, spin_e);
  }

  void finalize_elements ()
  {
    for (index_type element = 0; element < elements_count; element++)
      {
        dxs[element] = nodes_xs[elements_ends[element]] - nodes_xs[elements_starts[element]];
        dys[element] = nodes_ys[elements_ends[element]] - nodes_ys[elements_starts[element]];
        lens[element] = std::sqrt (dxs[element] * dxs[element] + dys[element] * dys[element]);
      }
  }

  void calculate_local_stiffness_matrices ()
  {
    if (use_frames)
      calculate_local_stiffness_matrices_frame ();
    else
      calculate_local_stiffness_matrices_beam ();
  }

  void calculate_local_stiffness_matrices_frame ()
  {
    std::array<data_type, stiffness_matrix_block_size * stiffness_matrix_block_size> k_prime;
    std::array<data_type, stiffness_matrix_block_size * stiffness_matrix_block_size> beta_prime;
    std::array<data_type, stiffness_matrix_block_size * stiffness_matrix_block_size> tmp_block;

    const data_type I = 700;

    for (index_type element = 0; element < elements_count; element++)
      {
        k_prime.fill (0.0);
        beta_prime.fill (0.0);

        const data_type L = lens[element];
        const data_type cos_theta  = dxs[element] / L;
        const data_type sin_theta  = dys[element] / L;
        const data_type ei_over_l3 = elements_e[element] * I / std::pow (L, 3);
        const data_type al2_over_I = elements_areas[element] * L * L / I;

        /// 0) We need to fill follow elements of local stiffness matrix
        ///
        ///             +--------------------------------+
        ///             | i\j |    0    |  1 |    2 |  3 |
        ///             +-----+---------+----+------+----+
        ///             | 0   | AL^2/I  |  0 |-AE/L |  0 |
        ///             +-----+---------+----+------+----+
        ///             | 1   |    0    |  0 |    0 |  0 |
        ///             +-----+---------+----+------+----+
        /// k = EI/L^3  | 2   |    0    |  0 | AE/L |  0 |
        ///             +-----+---------+----+------+----+
        ///             | 3   | -AL^2/I |  0 |    0 |  0 |
        ///             +-----------------------------+
        ///             | 4   |  0
        ///             +-----------------------------+
        ///             | 5   |  0
        ///             +-----------------------------+

        /// Row 1
        k_prime[0 * stiffness_matrix_block_size + 0] =  al2_over_I;
        k_prime[0 * stiffness_matrix_block_size + 3] = -al2_over_I;

        /// Row 2
        k_prime[1 * stiffness_matrix_block_size + 1] =  12.0f;
        k_prime[1 * stiffness_matrix_block_size + 2] =  6 * L;
        k_prime[1 * stiffness_matrix_block_size + 4] = -12.0f;
        k_prime[1 * stiffness_matrix_block_size + 5] =  6 * L;

        /// Row 3
        k_prime[2 * stiffness_matrix_block_size + 1] =  6 * L;
        k_prime[2 * stiffness_matrix_block_size + 2] =  4 * L * L;
        k_prime[2 * stiffness_matrix_block_size + 4] = -6 * L;
        k_prime[2 * stiffness_matrix_block_size + 5] =  2 * L * L;

        /// Row 4
        k_prime[3 * stiffness_matrix_block_size + 0] = -al2_over_I;
        k_prime[3 * stiffness_matrix_block_size + 3] =  al2_over_I;

        /// Row 5
        k_prime[4 * stiffness_matrix_block_size + 1] = -12.0f;
        k_prime[4 * stiffness_matrix_block_size + 2] = -6 * L;
        k_prime[4 * stiffness_matrix_block_size + 4] =  12.0f;
        k_prime[4 * stiffness_matrix_block_size + 5] = -6 * L;

        /// Row 6
        k_prime[5 * stiffness_matrix_block_size + 1] =  6 * L;
        k_prime[5 * stiffness_matrix_block_size + 2] =  2 * L * L;
        k_prime[5 * stiffness_matrix_block_size + 4] = -6 * L;
        k_prime[5 * stiffness_matrix_block_size + 5] =  4 * L * L;

        for (data_type &k_v: k_prime)
          k_v *= ei_over_l3;

        /// 1) We need to fill elements of rotation matrix beta prime
        ///
        ///   +-----------------------------+
        ///   | i\j |   0 |  1  |   2 |   3 |
        ///   +-----+-----+-----+-----+-----+
        ///   | 0   | cos | sin |   0 |   0 |
        ///   +-----+-----+-----+-----+-----+
        ///   | 1   |-sin | cos |   0 |   0 |
        ///   +-----+-----+-----+-----+-----+
        ///   | 2   |   0 |   0 | cos | sin |
        ///   +-----+-----+-----+-----+-----+
        ///   | 3   |   0 |   0 |-sin | cos |
        ///   +-----------------------------+

        beta_prime[0 * stiffness_matrix_block_size + 0] =  cos_theta;
        beta_prime[0 * stiffness_matrix_block_size + 1] =  sin_theta;
        beta_prime[1 * stiffness_matrix_block_size + 0] = -sin_theta;
        beta_prime[1 * stiffness_matrix_block_size + 1] =  cos_theta;
        beta_prime[2 * stiffness_matrix_block_size + 2] =  1.0f;

        beta_prime[3 * stiffness_matrix_block_size + 3] =  cos_theta;
        beta_prime[3 * stiffness_matrix_block_size + 4] =  sin_theta;
        beta_prime[4 * stiffness_matrix_block_size + 3] = -sin_theta;
        beta_prime[4 * stiffness_matrix_block_size + 4] =  cos_theta;
        beta_prime[5 * stiffness_matrix_block_size + 5] =  1.0f;

        /// 2) We need to calculate global stiffness matrix for this element: [k_e] = [Beta_e]^T [k^{'}_{e}] [Beta_e]
        matrix_mult_matrix (k_prime.data(), beta_prime.data(), tmp_block.data(), stiffness_matrix_block_size);
        matrix_transponse_and_mult (
          beta_prime.data(),
          tmp_block.data(),
          stiffness_matrix.get () + stiffness_matrix_block_size * stiffness_matrix_block_size * element,
          stiffness_matrix_block_size);
      }
  }


  void calculate_local_stiffness_matrices_beam ()
  {
    std::array<data_type, stiffness_matrix_block_size * stiffness_matrix_block_size> k_prime;
    std::array<data_type, stiffness_matrix_block_size * stiffness_matrix_block_size> beta_prime;
    std::array<data_type, stiffness_matrix_block_size * stiffness_matrix_block_size> tmp_block;

    for (index_type element = 0; element < elements_count; element++)
      {
        k_prime.fill (0.0);
        beta_prime.fill (0.0);

        const data_type cos_theta = dxs[element] / lens[element];
        const data_type sin_theta = dys[element] / lens[element];
        const data_type ae_over_l = elements_areas[element] * elements_e[element] / lens[element];

        /// 0) We need to fill follow elements of local stiffness matrix
        ///
        ///   +-----------------------------+
        ///   | i\j |    0 |  1 |    2 |  3 |
        ///   +-----+---- -+----+------+----+
        ///   | 0   | AE/L |  0 |-AE/L |  0 |
        ///   +-----+------+----+------+----+
        ///   | 1   |    0 |  0 |    0 |  0 |
        ///   +-----+------+----+------+----+
        ///   | 2   |-AE/L |  0 | AE/L |  0 |
        ///   +-----+------+----+------+----+
        ///   | 3   |    0 |  0 |    0 |  0 |
        ///   +-----------------------------+

        k_prime[0 * stiffness_matrix_block_size + 0] =  ae_over_l;
        k_prime[0 * stiffness_matrix_block_size + 2] = -ae_over_l;
        k_prime[2 * stiffness_matrix_block_size + 0] = -ae_over_l;
        k_prime[2 * stiffness_matrix_block_size + 2] =  ae_over_l;

        /// 1) We need to fill elements of rotation matrix beta prime
        ///
        ///   +-----------------------------+
        ///   | i\j |   0 |  1  |   2 |   3 |
        ///   +-----+-----+-----+-----+-----+
        ///   | 0   | cos | sin |   0 |   0 |
        ///   +-----+-----+-----+-----+-----+
        ///   | 1   |-sin | cos |   0 |   0 |
        ///   +-----+-----+-----+-----+-----+
        ///   | 2   |   0 |   0 | cos | sin |
        ///   +-----+-----+-----+-----+-----+
        ///   | 3   |   0 |   0 |-sin | cos |
        ///   +-----------------------------+

        beta_prime[0 * stiffness_matrix_block_size + 0] =  cos_theta;
        beta_prime[0 * stiffness_matrix_block_size + 1] =  sin_theta;
        beta_prime[1 * stiffness_matrix_block_size + 0] = -sin_theta;
        beta_prime[1 * stiffness_matrix_block_size + 1] =  cos_theta;

        beta_prime[2 * stiffness_matrix_block_size + 2] =  cos_theta;
        beta_prime[2 * stiffness_matrix_block_size + 3] =  sin_theta;
        beta_prime[3 * stiffness_matrix_block_size + 2] = -sin_theta;
        beta_prime[3 * stiffness_matrix_block_size + 3] =  cos_theta;

        /// 2) We need to calculate global stiffness matrix for this element: [k_e] = [Beta_e]^T [k^{'}_{e}] [Beta_e]
        matrix_mult_matrix (k_prime.data(), beta_prime.data(), tmp_block.data(), stiffness_matrix_block_size);
        matrix_transponse_and_mult (
          beta_prime.data(),
          tmp_block.data(),
          stiffness_matrix.get () + stiffness_matrix_block_size * stiffness_matrix_block_size * element,
          stiffness_matrix_block_size);
      }
  }

  void assemble_matrix ()
  {
    const index_type bs = use_frames ? 3 : 2;
    matrix = std::make_unique<bcsr_matrix_class<data_type, index_type>> (
      nodes_count /* n_rows */,
      nodes_count /* n_cols */,
      bs /* block_size - {x, y} for each node */,
      nodes_count + 2 * elements_count /* nnzb */); ///< Each element is presented by two entries in matrix (f->s, s->f) + diagonal elements

    std::fill_n (
      matrix->values.get (),
      matrix->size (),
      0.0);

    index_type *row_ptr = matrix->row_ptr.get ();
    std::fill_n (row_ptr, matrix->n_rows, 1); ///< Always count diagonal elements

    for (index_type element = 0; element < elements_count; element++)
      for (auto &node: {elements_starts[element], elements_ends[element]})
        row_ptr[node]++;

    {
      index_type offset = 0;
      for (index_type row = 0; row < matrix->n_rows; row++)
        {
          const index_type tmp = row_ptr[row];
          row_ptr[row] = offset;
          offset += tmp;
        }
      row_ptr[matrix->n_rows] = offset;
    }

    index_type *columns = matrix->columns.get ();

    for (index_type row = 0; row < matrix->n_rows; row++)
      columns[row_ptr[row]] = row; /// Add diag part

    std::unique_ptr<index_type[]> count (new index_type[nodes_count]);
    std::fill_n (count.get (), nodes_count, 1);

    for (index_type element = 0; element < elements_count; element++)
      {
        const index_type begin = elements_starts[element];
        const index_type end = elements_ends[element];

        columns[row_ptr[begin] + count[begin]++] = end; /// Cross-connection
        columns[row_ptr[end]   + count[end]++  ] = begin;
      }

    for (index_type row = 0; row < matrix->n_rows; row++)
      {
        if (count[row] != row_ptr[row + 1] - row_ptr[row])
          std::cerr << "Global stiffness matrix assembling is broken!" << std::endl;

        std::sort (columns + row_ptr[row], columns + row_ptr[row + 1]);
      }

    for (index_type element = 0; element < elements_count; element++)
      {
        const index_type begin = elements_starts[element];
        const index_type end = elements_ends[element];

        const data_type *stiffness = get_element_local_stiffness_matrix (element);

        int bc_begin[3] = {
          nodes_x_bc[begin],
          nodes_y_bc[begin],
          0
        };

        int bc_end[3] = {
          nodes_x_bc[end],
          nodes_y_bc[end],
          0
        };

        {

          data_type *diag = matrix->get_block_data_by_column (begin, begin);

          for (index_type i = 0; i < bs; i++)
            if (!bc_begin[i])
              for (index_type j = 0; j < bs; j++)
                if (!bc_begin[j])
                  diag[i * bs + j] += stiffness[i * stiffness_matrix_block_size + j];

          data_type *off_diag = matrix->get_block_data_by_column (begin, end);
          for (index_type i = 0; i < bs; i++)
            if (!bc_begin[i])
              for (index_type j = 0; j < bs; j++)
                if (!bc_end[j])
                  off_diag[i * bs + j] = stiffness[i * stiffness_matrix_block_size + j + bs];
        }

        {
          data_type *diag = matrix->get_block_data_by_column (end, end);

          for (index_type i = 0; i < bs; i++)
            if (!bc_end[i])
              for (index_type j = 0; j < bs; j++)
                if (!bc_end[j])
                  diag[i * bs + j] += stiffness[stiffness_matrix_block_size * bs + i * stiffness_matrix_block_size + j + bs];

          data_type *off_diag = matrix->get_block_data_by_column (end, begin);
          for (index_type i = 0; i < bs; i++)
            if (!bc_end[i])
              for (index_type j = 0; j < bs; j++)
                if (!bc_begin[j])
                  off_diag[i * bs + j] = stiffness[stiffness_matrix_block_size * bs + i * stiffness_matrix_block_size + j];
        }
      }
  }

private:
  data_type *get_element_local_stiffness_matrix (index_type element)
  {
    return stiffness_matrix.get () + element * stiffness_matrix_block_size * stiffness_matrix_block_size;
  }

private:
  index_type nodes_count {};
  std::unique_ptr<data_type[]> nodes_xs;
  std::unique_ptr<data_type[]> nodes_ys;

  std::unique_ptr<int[]> nodes_x_bc;
  std::unique_ptr<int[]> nodes_y_bc;

  std::unique_ptr<data_type[]> elements_areas;
  std::unique_ptr<data_type[]> elements_e;

  std::unique_ptr<data_type[]> dxs;  ///< Higher node id minus lower node id!
  std::unique_ptr<data_type[]> dys;  ///< Higher node id minus lower node id!
  std::unique_ptr<data_type[]> lens; ///< Higher node id minus lower node id!

  std::unique_ptr<data_type[]> stiffness_matrix;

  std::unique_ptr<index_type[]> elements_starts;
  std::unique_ptr<index_type[]> elements_ends;

public:
  std::unique_ptr<bcsr_matrix_class<data_type, index_type>> matrix;
  std::unique_ptr<data_type[]> forces_rhs;
};

#endif //BLOCK_MATRIX_FORMAT_PERFORMANCE_GOLDEN_GATE_BRIDGE_H
