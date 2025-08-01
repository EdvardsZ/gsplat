#include <ATen/TensorUtils.h>
#include <ATen/core/Tensor.h>
#include <c10/cuda/CUDAGuard.h> // for DEVICE_GUARD
#include <tuple>

#include <ATen/Functions.h>
#include <ATen/NativeFunctions.h>

#include "Common.h"
#include "Ops.h"
#include "Rasterization.h"
#include "Cameras.h"

namespace gsplat {

////////////////////////////////////////////////////
// 3DGS
////////////////////////////////////////////////////

std::tuple<at::Tensor, at::Tensor, at::Tensor> rasterize_to_pixels_3dgs_fwd(
    // Gaussian parameters
    const at::Tensor means2d,   // [..., N, 2] or [nnz, 2]
    const at::Tensor conics,    // [..., N, 3] or [nnz, 3]
    const at::Tensor colors,    // [..., N, channels] or [nnz, channels]
    const at::Tensor opacities, // [..., N]  or [nnz]
    const at::optional<at::Tensor> backgrounds, // [..., channels]
    const at::optional<at::Tensor> masks,       // [..., tile_height, tile_width]
    // image size
    const uint32_t image_width,
    const uint32_t image_height,
    const uint32_t tile_size,
    // intersections
    const at::Tensor tile_offsets, // [..., tile_height, tile_width]
    const at::Tensor flatten_ids   // [n_isects]
) {
    DEVICE_GUARD(means2d);
    CHECK_INPUT(means2d);
    CHECK_INPUT(conics);
    CHECK_INPUT(colors);
    CHECK_INPUT(opacities);
    CHECK_INPUT(tile_offsets);
    CHECK_INPUT(flatten_ids);
    if (backgrounds.has_value()) {
        CHECK_INPUT(backgrounds.value());
    }
    if (masks.has_value()) {
        CHECK_INPUT(masks.value());
    }

    auto opt = means2d.options();
    at::DimVector image_dims(tile_offsets.sizes().slice(0, tile_offsets.dim() - 2));
    uint32_t channels = colors.size(-1);

    at::DimVector renders_dims(image_dims);
    renders_dims.append({image_height, image_width, channels});
    at::Tensor renders = at::empty(renders_dims, opt);

    at::DimVector alphas_dims(image_dims);
    alphas_dims.append({image_height, image_width, 1});
    at::Tensor alphas = at::empty(alphas_dims, opt);

    at::DimVector last_ids_dims(image_dims);
    last_ids_dims.append({image_height, image_width});
    at::Tensor last_ids = at::empty(last_ids_dims, opt.dtype(at::kInt));

#define __LAUNCH_KERNEL__(N)                                                   \
    case N:                                                                    \
        launch_rasterize_to_pixels_3dgs_fwd_kernel<N>(                         \
            means2d,                                                           \
            conics,                                                            \
            colors,                                                            \
            opacities,                                                         \
            backgrounds,                                                       \
            masks,                                                             \
            image_width,                                                       \
            image_height,                                                      \
            tile_size,                                                         \
            tile_offsets,                                                      \
            flatten_ids,                                                       \
            renders,                                                           \
            alphas,                                                            \
            last_ids                                                           \
        );                                                                     \
        break;

    // TODO: an optimization can be done by passing the actual number of
    // channels into the kernel functions and avoid necessary global memory
    // writes. This requires moving the channel padding from python to C side.
    switch (channels) {
        __LAUNCH_KERNEL__(1)
        __LAUNCH_KERNEL__(2)
        __LAUNCH_KERNEL__(3)
        __LAUNCH_KERNEL__(4)
        __LAUNCH_KERNEL__(5)
        __LAUNCH_KERNEL__(8)
        __LAUNCH_KERNEL__(9)
        __LAUNCH_KERNEL__(16)
        __LAUNCH_KERNEL__(17)
        __LAUNCH_KERNEL__(32)
        __LAUNCH_KERNEL__(33)
        __LAUNCH_KERNEL__(64)
        __LAUNCH_KERNEL__(65)
        __LAUNCH_KERNEL__(128)
        __LAUNCH_KERNEL__(129)
        __LAUNCH_KERNEL__(256)
        __LAUNCH_KERNEL__(257)
        __LAUNCH_KERNEL__(512)
        __LAUNCH_KERNEL__(513)
    default:
        AT_ERROR("Unsupported number of channels: ", channels);
    }
#undef __LAUNCH_KERNEL__

    return std::make_tuple(renders, alphas, last_ids);
}

std::tuple<at::Tensor, at::Tensor, at::Tensor, at::Tensor, at::Tensor>
rasterize_to_pixels_3dgs_bwd(
    // Gaussian parameters
    const at::Tensor means2d,                   // [..., N, 2] or [nnz, 2]
    const at::Tensor conics,                    // [..., N, 3] or [nnz, 3]
    const at::Tensor colors,                    // [..., N, channels] or [nnz, channels]
    const at::Tensor opacities,                 // [..., N] or [nnz]
    const at::optional<at::Tensor> backgrounds, // [..., channels]
    const at::optional<at::Tensor> masks,       // [..., tile_height, tile_width]
    // image size
    const uint32_t image_width,
    const uint32_t image_height,
    const uint32_t tile_size,
    // intersections
    const at::Tensor tile_offsets, // [..., tile_height, tile_width]
    const at::Tensor flatten_ids,  // [n_isects]
    // forward outputs
    const at::Tensor render_alphas, // [..., image_height, image_width, 1]
    const at::Tensor last_ids,      // [..., image_height, image_width]
    // gradients of outputs
    const at::Tensor v_render_colors, // [..., image_height, image_width, channels]
    const at::Tensor v_render_alphas, // [..., image_height, image_width, 1]
    // options
    bool absgrad
) {
    DEVICE_GUARD(means2d);
    CHECK_INPUT(means2d);
    CHECK_INPUT(conics);
    CHECK_INPUT(colors);
    CHECK_INPUT(opacities);
    CHECK_INPUT(tile_offsets);
    CHECK_INPUT(flatten_ids);
    CHECK_INPUT(render_alphas);
    CHECK_INPUT(last_ids);
    CHECK_INPUT(v_render_colors);
    CHECK_INPUT(v_render_alphas);
    if (backgrounds.has_value()) {
        CHECK_INPUT(backgrounds.value());
    }
    if (masks.has_value()) {
        CHECK_INPUT(masks.value());
    }

    uint32_t channels = colors.size(-1);

    at::Tensor v_means2d = at::zeros_like(means2d);
    at::Tensor v_conics = at::zeros_like(conics);
    at::Tensor v_colors = at::zeros_like(colors);
    at::Tensor v_opacities = at::zeros_like(opacities);
    at::Tensor v_means2d_abs;
    if (absgrad) {
        v_means2d_abs = at::zeros_like(means2d);
    }

#define __LAUNCH_KERNEL__(N)                                                   \
    case N:                                                                    \
        launch_rasterize_to_pixels_3dgs_bwd_kernel<N>(                         \
            means2d,                                                           \
            conics,                                                            \
            colors,                                                            \
            opacities,                                                         \
            backgrounds,                                                       \
            masks,                                                             \
            image_width,                                                       \
            image_height,                                                      \
            tile_size,                                                         \
            tile_offsets,                                                      \
            flatten_ids,                                                       \
            render_alphas,                                                     \
            last_ids,                                                          \
            v_render_colors,                                                   \
            v_render_alphas,                                                   \
            absgrad ? c10::optional<at::Tensor>(v_means2d_abs) : c10::nullopt, \
            v_means2d,                                                         \
            v_conics,                                                          \
            v_colors,                                                          \
            v_opacities                                                        \
        );                                                                     \
        break;

    // TODO: an optimization can be done by passing the actual number of
    // channels into the kernel functions and avoid necessary global memory
    // writes. This requires moving the channel padding from python to C side.
    switch (channels) {
        __LAUNCH_KERNEL__(1)
        __LAUNCH_KERNEL__(2)
        __LAUNCH_KERNEL__(3)
        __LAUNCH_KERNEL__(4)
        __LAUNCH_KERNEL__(5)
        __LAUNCH_KERNEL__(8)
        __LAUNCH_KERNEL__(9)
        __LAUNCH_KERNEL__(16)
        __LAUNCH_KERNEL__(17)
        __LAUNCH_KERNEL__(32)
        __LAUNCH_KERNEL__(33)
        __LAUNCH_KERNEL__(64)
        __LAUNCH_KERNEL__(65)
        __LAUNCH_KERNEL__(128)
        __LAUNCH_KERNEL__(129)
        __LAUNCH_KERNEL__(256)
        __LAUNCH_KERNEL__(257)
        __LAUNCH_KERNEL__(512)
        __LAUNCH_KERNEL__(513)
    default:
        AT_ERROR("Unsupported number of channels: ", channels);
    }
#undef __LAUNCH_KERNEL__

    return std::make_tuple(
        v_means2d_abs, v_means2d, v_conics, v_colors, v_opacities
    );
}

std::tuple<at::Tensor, at::Tensor> rasterize_to_indices_3dgs(
    const uint32_t range_start,
    const uint32_t range_end,        // iteration steps
    const at::Tensor transmittances, // [..., image_height, image_width]
    // Gaussian parameters
    const at::Tensor means2d,   // [..., N, 2]
    const at::Tensor conics,    // [..., N, 3]
    const at::Tensor opacities, // [..., N]
    // image size
    const uint32_t image_width,
    const uint32_t image_height,
    const uint32_t tile_size,
    // intersections
    const at::Tensor tile_offsets, // [..., tile_height, tile_width]
    const at::Tensor flatten_ids   // [n_isects]
) {
    DEVICE_GUARD(means2d);
    CHECK_INPUT(means2d);
    CHECK_INPUT(conics);
    CHECK_INPUT(opacities);
    CHECK_INPUT(tile_offsets);
    CHECK_INPUT(flatten_ids);

    auto opt = means2d.options();
    uint32_t N = means2d.size(-2); // number of gaussians
    uint32_t I = means2d.numel() / (2 * N); // number of images

    uint32_t n_isects = flatten_ids.size(0);

    // First pass: count the number of gaussians that contribute to each pixel
    int64_t n_elems;
    at::Tensor chunk_starts;
    if (n_isects) {
        at::Tensor chunk_cnts = at::zeros(
            {I * image_height * image_width}, opt.dtype(at::kInt)
        );
        launch_rasterize_to_indices_3dgs_kernel(
            range_start,
            range_end,
            transmittances,
            means2d,
            conics,
            opacities,
            image_width,
            image_height,
            tile_size,
            tile_offsets,
            flatten_ids,
            c10::nullopt, // chunk_starts
            at::optional<at::Tensor>(chunk_cnts),
            c10::nullopt, // gaussian_ids
            c10::nullopt  // pixel_ids
        );
        at::Tensor cumsum = at::cumsum(chunk_cnts, 0, chunk_cnts.scalar_type());
        n_elems = cumsum[-1].item<int64_t>();
        chunk_starts = at::sub(cumsum, chunk_cnts);
    } else {
        n_elems = 0;
    }

    // Second pass: allocate memory and write out the gaussian and pixel ids.
    at::Tensor gaussian_ids = at::empty({n_elems}, opt.dtype(at::kLong));
    at::Tensor pixel_ids = at::empty({n_elems}, opt.dtype(at::kLong));
    if (n_elems) {
        launch_rasterize_to_indices_3dgs_kernel(
            range_start,
            range_end,
            transmittances,
            means2d,
            conics,
            opacities,
            image_width,
            image_height,
            tile_size,
            tile_offsets,
            flatten_ids,
            at::optional<at::Tensor>(chunk_starts),
            c10::nullopt, // chunk_cnts
            at::optional<at::Tensor>(gaussian_ids),
            at::optional<at::Tensor>(pixel_ids)
        );
    }
    return std::make_tuple(gaussian_ids, pixel_ids);
}

////////////////////////////////////////////////////
// 2DGS
////////////////////////////////////////////////////

std::tuple<
    at::Tensor,
    at::Tensor,
    at::Tensor,
    at::Tensor,
    at::Tensor,
    at::Tensor,
    at::Tensor>
rasterize_to_pixels_2dgs_fwd(
    // Gaussian parameters
    const at::Tensor means2d,        // [..., N, 2] or [nnz, 2]
    const at::Tensor ray_transforms, // [..., N, 3, 3] or [nnz, 3, 3]
    const at::Tensor colors,         // [..., N, channels] or [nnz, channels]
    const at::Tensor opacities,      // [..., N]  or [nnz]
    const at::Tensor normals,        // [..., N, 3] or [nnz, 3]
    const at::optional<at::Tensor> backgrounds, // [..., channels]
    const at::optional<at::Tensor> masks,       // [..., tile_height, tile_width]
    // image size
    const uint32_t image_width,
    const uint32_t image_height,
    const uint32_t tile_size,
    // intersections
    const at::Tensor tile_offsets, // [..., tile_height, tile_width]
    const at::Tensor flatten_ids   // [n_isects]
) {
    DEVICE_GUARD(means2d);
    CHECK_INPUT(means2d);
    CHECK_INPUT(ray_transforms);
    CHECK_INPUT(colors);
    CHECK_INPUT(opacities);
    CHECK_INPUT(normals);
    CHECK_INPUT(tile_offsets);
    CHECK_INPUT(flatten_ids);
    if (backgrounds.has_value()) {
        CHECK_INPUT(backgrounds.value());
    }
    if (masks.has_value()) {
        CHECK_INPUT(masks.value());
    }
    auto opt = means2d.options();

    at::DimVector image_dims(tile_offsets.sizes().slice(0, tile_offsets.dim() - 2));
    uint32_t channels = colors.size(-1);

    at::DimVector renders_dims(image_dims);
    renders_dims.append({image_height, image_width, channels});
    at::Tensor renders = at::empty(renders_dims, opt);

    at::DimVector alphas_dims(image_dims);
    alphas_dims.append({image_height, image_width, 1});
    at::Tensor alphas = at::empty(alphas_dims, opt);

    at::DimVector last_ids_dims(image_dims);
    last_ids_dims.append({image_height, image_width});
    at::Tensor last_ids = at::empty(last_ids_dims, opt.dtype(at::kInt));

    at::DimVector median_ids_dims(image_dims);
    median_ids_dims.append({image_height, image_width});
    at::Tensor median_ids = at::empty(median_ids_dims, opt.dtype(at::kInt));

    at::DimVector render_normals_dims(image_dims);
    render_normals_dims.append({image_height, image_width, 3});
    at::Tensor render_normals = at::empty(render_normals_dims, opt);

    at::DimVector render_distort_dims(image_dims);
    render_distort_dims.append({image_height, image_width, 1});
    at::Tensor render_distort = at::empty(render_distort_dims, opt);

    at::DimVector render_median_dims(image_dims);
    render_median_dims.append({image_height, image_width, 1});
    at::Tensor render_median = at::empty(render_median_dims, opt);

#define __LAUNCH_KERNEL__(N)                                                   \
    case N:                                                                    \
        launch_rasterize_to_pixels_2dgs_fwd_kernel<N>(                         \
            means2d,                                                           \
            ray_transforms,                                                    \
            colors,                                                            \
            opacities,                                                         \
            normals,                                                           \
            backgrounds,                                                       \
            masks,                                                             \
            image_width,                                                       \
            image_height,                                                      \
            tile_size,                                                         \
            tile_offsets,                                                      \
            flatten_ids,                                                       \
            renders,                                                           \
            alphas,                                                            \
            render_normals,                                                    \
            render_distort,                                                    \
            render_median,                                                     \
            last_ids,                                                          \
            median_ids                                                         \
        );                                                                     \
        break;

    // TODO: an optimization can be done by passing the actual number of
    // channels into the kernel functions and avoid necessary global memory
    // writes. This requires moving the channel padding from python to C side.
    switch (channels) {
        __LAUNCH_KERNEL__(1)
        __LAUNCH_KERNEL__(2)
        __LAUNCH_KERNEL__(3)
        __LAUNCH_KERNEL__(4)
        __LAUNCH_KERNEL__(5)
        __LAUNCH_KERNEL__(8)
        __LAUNCH_KERNEL__(9)
        __LAUNCH_KERNEL__(16)
        __LAUNCH_KERNEL__(17)
        __LAUNCH_KERNEL__(32)
        __LAUNCH_KERNEL__(33)
        __LAUNCH_KERNEL__(64)
        __LAUNCH_KERNEL__(65)
        __LAUNCH_KERNEL__(128)
        __LAUNCH_KERNEL__(129)
        __LAUNCH_KERNEL__(256)
        __LAUNCH_KERNEL__(257)
        __LAUNCH_KERNEL__(512)
        __LAUNCH_KERNEL__(513)
    default:
        AT_ERROR("Unsupported number of channels: ", channels);
    }
#undef __LAUNCH_KERNEL__

    return std::make_tuple(
        renders,
        alphas,
        render_normals,
        render_distort,
        render_median,
        last_ids,
        median_ids
    );
}

std::tuple<
    at::Tensor,
    at::Tensor,
    at::Tensor,
    at::Tensor,
    at::Tensor,
    at::Tensor,
    at::Tensor>
rasterize_to_pixels_2dgs_bwd(
    // Gaussian parameters
    const at::Tensor means2d,        // [..., N, 2] or [nnz, 2]
    const at::Tensor ray_transforms, // [..., N, 3, 3] or [nnz, 3, 3]
    const at::Tensor colors,         // [..., N, channels] or [nnz, channels]
    const at::Tensor opacities,      // [..., N] or [nnz]
    const at::Tensor normals,        // [..., N, 3] or [nnz, 3]
    const at::Tensor densify,
    const at::optional<at::Tensor> backgrounds, // [..., channels]
    const at::optional<at::Tensor> masks,       // [..., tile_height, tile_width]
    // image size
    const uint32_t image_width,
    const uint32_t image_height,
    const uint32_t tile_size,
    // ray_crossions
    const at::Tensor tile_offsets, // [..., tile_height, tile_width]
    const at::Tensor flatten_ids,  // [n_isects]
    // forward outputs
    const at::Tensor render_colors, // [..., image_height, image_width, channels]
    const at::Tensor render_alphas, // [..., image_height, image_width, 1]
    const at::Tensor last_ids,      // [..., image_height, image_width]
    const at::Tensor median_ids,    // [..., image_height, image_width]
    // gradients of outputs
    const at::Tensor v_render_colors,  // [..., image_height, image_width, channels]
    const at::Tensor v_render_alphas,  // [..., image_height, image_width, 1]
    const at::Tensor v_render_normals, // [..., image_height, image_width, 3]
    const at::Tensor v_render_distort, // [..., image_height, image_width, 1]
    const at::Tensor v_render_median,  // [..., image_height, image_width, 1]
    // options
    bool absgrad
) {
    DEVICE_GUARD(means2d);
    CHECK_INPUT(means2d);
    CHECK_INPUT(ray_transforms);
    CHECK_INPUT(colors);
    CHECK_INPUT(opacities);
    CHECK_INPUT(normals);
    CHECK_INPUT(densify);
    CHECK_INPUT(tile_offsets);
    CHECK_INPUT(flatten_ids);
    CHECK_INPUT(render_colors);
    CHECK_INPUT(render_alphas);
    CHECK_INPUT(last_ids);
    CHECK_INPUT(median_ids);
    CHECK_INPUT(v_render_colors);
    CHECK_INPUT(v_render_alphas);
    CHECK_INPUT(v_render_normals);
    CHECK_INPUT(v_render_distort);
    CHECK_INPUT(v_render_median);
    if (backgrounds.has_value()) {
        CHECK_INPUT(backgrounds.value());
    }
    if (masks.has_value()) {
        CHECK_INPUT(masks.value());
    }

    uint32_t channels = colors.size(-1);

    at::Tensor v_means2d = at::zeros_like(means2d);
    at::Tensor v_ray_transforms = at::zeros_like(ray_transforms);
    at::Tensor v_colors = at::zeros_like(colors);
    at::Tensor v_normals = at::zeros_like(normals);
    at::Tensor v_opacities = at::zeros_like(opacities);
    at::Tensor v_means2d_abs;
    if (absgrad) {
        v_means2d_abs = at::zeros_like(means2d);
    }
    at::Tensor v_densify = at::zeros_like(densify);

#define __LAUNCH_KERNEL__(N)                                                   \
    case N:                                                                    \
        launch_rasterize_to_pixels_2dgs_bwd_kernel<N>(                         \
            means2d,                                                           \
            ray_transforms,                                                    \
            colors,                                                            \
            opacities,                                                         \
            normals,                                                           \
            densify,                                                           \
            backgrounds,                                                       \
            masks,                                                             \
            image_width,                                                       \
            image_height,                                                      \
            tile_size,                                                         \
            tile_offsets,                                                      \
            flatten_ids,                                                       \
            render_colors,                                                     \
            render_alphas,                                                     \
            last_ids,                                                          \
            median_ids,                                                        \
            v_render_colors,                                                   \
            v_render_alphas,                                                   \
            v_render_normals,                                                  \
            v_render_distort,                                                  \
            v_render_median,                                                   \
            absgrad ? c10::optional<at::Tensor>(v_means2d_abs) : c10::nullopt, \
            v_means2d,                                                         \
            v_ray_transforms,                                                  \
            v_colors,                                                          \
            v_opacities,                                                       \
            v_normals,                                                         \
            v_densify                                                          \
        );                                                                     \
        break;

    // TODO: an optimization can be done by passing the actual number of
    // channels into the kernel functions and avoid necessary global memory
    // writes. This requires moving the channel padding from python to C side.
    switch (channels) {
        __LAUNCH_KERNEL__(1)
        __LAUNCH_KERNEL__(2)
        __LAUNCH_KERNEL__(3)
        __LAUNCH_KERNEL__(4)
        __LAUNCH_KERNEL__(5)
        __LAUNCH_KERNEL__(8)
        __LAUNCH_KERNEL__(9)
        __LAUNCH_KERNEL__(16)
        __LAUNCH_KERNEL__(17)
        __LAUNCH_KERNEL__(32)
        __LAUNCH_KERNEL__(33)
        __LAUNCH_KERNEL__(64)
        __LAUNCH_KERNEL__(65)
        __LAUNCH_KERNEL__(128)
        __LAUNCH_KERNEL__(129)
        __LAUNCH_KERNEL__(256)
        __LAUNCH_KERNEL__(257)
        __LAUNCH_KERNEL__(512)
        __LAUNCH_KERNEL__(513)
    default:
        AT_ERROR("Unsupported number of channels: ", channels);
    }
#undef __LAUNCH_KERNEL__

    return std::make_tuple(
        v_means2d_abs,
        v_means2d,
        v_ray_transforms,
        v_colors,
        v_opacities,
        v_normals,
        v_densify
    );
}

std::tuple<at::Tensor, at::Tensor> rasterize_to_indices_2dgs(
    const uint32_t range_start,
    const uint32_t range_end,        // iteration steps
    const at::Tensor transmittances, // [..., image_height, image_width]
    // Gaussian parameters
    const at::Tensor means2d,        // [..., N, 2]
    const at::Tensor ray_transforms, // [..., N, 3, 3]
    const at::Tensor opacities,      // [..., N]
    // image size
    const uint32_t image_width,
    const uint32_t image_height,
    const uint32_t tile_size,
    // intersections
    const at::Tensor tile_offsets, // [..., tile_height, tile_width]
    const at::Tensor flatten_ids   // [n_isects]
) {
    DEVICE_GUARD(means2d);
    CHECK_INPUT(means2d);
    CHECK_INPUT(ray_transforms);
    CHECK_INPUT(opacities);
    CHECK_INPUT(tile_offsets);
    CHECK_INPUT(flatten_ids);

    auto opt = means2d.options();
    uint32_t N = means2d.size(-2); // number of gaussians
    uint32_t I = means2d.numel() / (2 * N); // number of images

    uint32_t n_isects = flatten_ids.size(0);

    // First pass: count the number of gaussians that contribute to each pixel
    int64_t n_elems;
    at::Tensor chunk_starts;
    if (n_isects) {
        at::Tensor chunk_cnts = at::zeros(
            {I * image_height * image_width}, opt.dtype(at::kInt)
        );
        launch_rasterize_to_indices_2dgs_kernel(
            range_start,
            range_end,
            transmittances,
            means2d,
            ray_transforms,
            opacities,
            image_width,
            image_height,
            tile_size,
            tile_offsets,
            flatten_ids,
            c10::nullopt, // chunk_starts
            at::optional<at::Tensor>(chunk_cnts),
            c10::nullopt, // gaussian_ids
            c10::nullopt  // pixel_ids
        );
        at::Tensor cumsum = at::cumsum(chunk_cnts, 0, chunk_cnts.scalar_type());
        n_elems = cumsum[-1].item<int64_t>();
        chunk_starts = at::sub(cumsum, chunk_cnts);
    } else {
        n_elems = 0;
    }

    // Second pass: allocate memory and write out the gaussian and pixel ids.
    at::Tensor gaussian_ids = at::empty({n_elems}, opt.dtype(at::kLong));
    at::Tensor pixel_ids = at::empty({n_elems}, opt.dtype(at::kLong));
    if (n_elems) {
        launch_rasterize_to_indices_2dgs_kernel(
            range_start,
            range_end,
            transmittances,
            means2d,
            ray_transforms,
            opacities,
            image_width,
            image_height,
            tile_size,
            tile_offsets,
            flatten_ids,
            at::optional<at::Tensor>(chunk_starts),
            c10::nullopt, // chunk_cnts
            at::optional<at::Tensor>(gaussian_ids),
            at::optional<at::Tensor>(pixel_ids)
        );
    }
    return std::make_tuple(gaussian_ids, pixel_ids);
}

////////////////////////////////////////////////////
// 3DGS (from world)
////////////////////////////////////////////////////

std::tuple<at::Tensor, at::Tensor, at::Tensor> rasterize_to_pixels_from_world_3dgs_fwd(
    // Gaussian parameters
    const at::Tensor means,     // [..., N, 3]
    const at::Tensor quats,     // [..., N, 4]
    const at::Tensor scales,    // [..., N, 3]
    const at::Tensor colors,    // [..., C, N, channels] or [nnz, channels]
    const at::Tensor opacities, // [..., C, N] or [nnz]
    const at::optional<at::Tensor> backgrounds, // [..., C, channels]
    const at::optional<at::Tensor> masks,       // [..., C, tile_height, tile_width]
    // image size
    const uint32_t image_width,
    const uint32_t image_height,
    const uint32_t tile_size,
    // camera
    const at::Tensor viewmats0,               // [..., C, 4, 4]
    const at::optional<at::Tensor> viewmats1, // [..., C, 4, 4] optional for rolling shutter
    const at::Tensor Ks,                      // [..., C, 3, 3]
    const CameraModelType camera_model,
    // uncented transform
    const UnscentedTransformParameters ut_params,
    ShutterType rs_type,
    const at::optional<at::Tensor> radial_coeffs,     // [..., C, 6] or [..., C, 4] optional
    const at::optional<at::Tensor> tangential_coeffs, // [..., C, 2] optional
    const at::optional<at::Tensor> thin_prism_coeffs, // [..., C, 4] optional
    const FThetaCameraDistortionParameters ftheta_coeffs, // shared parameters for all cameras
    // intersections
    const at::Tensor tile_offsets, // [..., C, tile_height, tile_width]
    const at::Tensor flatten_ids   // [n_isects]
) {
    DEVICE_GUARD(means);
    CHECK_INPUT(means);
    CHECK_INPUT(quats);
    CHECK_INPUT(scales);
    CHECK_INPUT(colors);
    CHECK_INPUT(opacities);
    CHECK_INPUT(tile_offsets);
    CHECK_INPUT(flatten_ids);
    if (backgrounds.has_value()) {
        CHECK_INPUT(backgrounds.value());
    }
    if (masks.has_value()) {
        CHECK_INPUT(masks.value());
    }
    
    auto opt = means.options();
    at::DimVector batch_dims(means.sizes().slice(0, means.dim() - 2));
    uint32_t C = viewmats0.size(-3);     // number of cameras
    // uint32_t N = means.size(-2);         // number of gaussians
    uint32_t channels = colors.size(-1);
    assert (channels == 3); // only support RGB for now

    at::DimVector renders_shape(batch_dims);
    renders_shape.append({C, image_height, image_width, channels});
    at::Tensor renders = at::empty(renders_shape, opt);

    at::DimVector alphas_shape(batch_dims);
    alphas_shape.append({C, image_height, image_width, 1});
    at::Tensor alphas = at::empty(alphas_shape, opt);

    at::DimVector last_ids_shape(batch_dims);
    last_ids_shape.append({C, image_height, image_width});
    at::Tensor last_ids = at::empty(last_ids_shape, opt.dtype(at::kInt));

#define __LAUNCH_KERNEL__(N)                                                   \
    case N:                                                                    \
        launch_rasterize_to_pixels_from_world_3dgs_fwd_kernel<N>(              \
            means,                                                             \
            quats,                                                             \
            scales,                                                            \
            colors,                                                            \
            opacities,                                                         \
            backgrounds,                                                       \
            masks,                                                             \
            image_width,                                                       \
            image_height,                                                      \
            tile_size,                                                         \
            viewmats0,                                                         \
            viewmats1,                                                         \
            Ks,                                                                \
            camera_model,                                                      \
            ut_params,                                                         \
            rs_type,                                                           \
            radial_coeffs,                                                     \
            tangential_coeffs,                                                 \
            thin_prism_coeffs,                                                 \
            ftheta_coeffs,                                                     \
            tile_offsets,                                                      \
            flatten_ids,                                                       \
            renders,                                                           \
            alphas,                                                            \
            last_ids                                                           \
        );                                                                     \
        break;

    // TODO: an optimization can be done by passing the actual number of
    // channels into the kernel functions and avoid necessary global memory
    // writes. This requires moving the channel padding from python to C side.
    switch (channels) {
        __LAUNCH_KERNEL__(1)
        __LAUNCH_KERNEL__(2)
        __LAUNCH_KERNEL__(3)
        __LAUNCH_KERNEL__(4)
        __LAUNCH_KERNEL__(5)
        __LAUNCH_KERNEL__(8)
        __LAUNCH_KERNEL__(9)
        __LAUNCH_KERNEL__(16)
        __LAUNCH_KERNEL__(17)
        __LAUNCH_KERNEL__(32)
        __LAUNCH_KERNEL__(33)
        __LAUNCH_KERNEL__(64)
        __LAUNCH_KERNEL__(65)
        __LAUNCH_KERNEL__(128)
        __LAUNCH_KERNEL__(129)
        __LAUNCH_KERNEL__(256)
        __LAUNCH_KERNEL__(257)
        __LAUNCH_KERNEL__(512)
        __LAUNCH_KERNEL__(513)
    default:
        AT_ERROR("Unsupported number of channels: ", channels);
    }
#undef __LAUNCH_KERNEL__

    return std::make_tuple(renders, alphas, last_ids);
};


std::tuple<at::Tensor, at::Tensor, at::Tensor, at::Tensor, at::Tensor>
rasterize_to_pixels_from_world_3dgs_bwd(
    // Gaussian parameters
    const at::Tensor means,  // [..., N, 3]
    const at::Tensor quats,  // [..., N, 4]
    const at::Tensor scales, // [..., N, 3]
    const at::Tensor colors,                    // [..., C, N, 3] or [nnz, 3]
    const at::Tensor opacities,                 // [..., C, N] or [nnz]
    const at::optional<at::Tensor> backgrounds, // [..., C, 3]
    const at::optional<at::Tensor> masks,       // [..., C, tile_height, tile_width]
    // image size
    const uint32_t image_width,
    const uint32_t image_height,
    const uint32_t tile_size,
    // camera
    const at::Tensor viewmats0,               // [..., C, 4, 4]
    const at::optional<at::Tensor> viewmats1, // [..., C, 4, 4] optional for rolling shutter
    const at::Tensor Ks,                      // [..., C, 3, 3]
    const CameraModelType camera_model,
    // uncented transform
    const UnscentedTransformParameters ut_params,
    ShutterType rs_type,
    const at::optional<at::Tensor> radial_coeffs,     // [..., C, 6] or [..., C, 4] optional
    const at::optional<at::Tensor> tangential_coeffs, // [..., C, 2] optional
    const at::optional<at::Tensor> thin_prism_coeffs, // [..., C, 4] optional
    const FThetaCameraDistortionParameters ftheta_coeffs, // shared parameters for all cameras
    // intersections
    const at::Tensor tile_offsets, // [..., C, tile_height, tile_width]
    const at::Tensor flatten_ids,  // [n_isects]
    // forward outputs
    const at::Tensor render_alphas, // [..., C, image_height, image_width, 1]
    const at::Tensor last_ids,      // [..., C, image_height, image_width]
    // gradients of outputs
    const at::Tensor v_render_colors, // [..., C, image_height, image_width, 3]
    const at::Tensor v_render_alphas // [..., C, image_height, image_width, 1]
) {
    DEVICE_GUARD(means);
    CHECK_INPUT(means);
    CHECK_INPUT(quats);
    CHECK_INPUT(scales);
    CHECK_INPUT(colors);
    CHECK_INPUT(opacities);
    CHECK_INPUT(tile_offsets);
    CHECK_INPUT(flatten_ids);
    CHECK_INPUT(render_alphas);
    CHECK_INPUT(last_ids);
    CHECK_INPUT(v_render_colors);
    CHECK_INPUT(v_render_alphas);
    if (backgrounds.has_value()) {
        CHECK_INPUT(backgrounds.value());
    }
    if (masks.has_value()) {
        CHECK_INPUT(masks.value());
    }

    uint32_t channels = colors.size(-1);

    at::Tensor v_means = at::zeros_like(means);
    at::Tensor v_quats = at::zeros_like(quats);
    at::Tensor v_scales = at::zeros_like(scales);
    at::Tensor v_colors = at::zeros_like(colors);
    at::Tensor v_opacities = at::zeros_like(opacities);

#define __LAUNCH_KERNEL__(N)                                                   \
    case N:                                                                    \
        launch_rasterize_to_pixels_from_world_3dgs_bwd_kernel<N>(              \
            means,                                                             \
            quats,                                                             \
            scales,                                                            \
            colors,                                                            \
            opacities,                                                         \
            backgrounds,                                                       \
            masks,                                                             \
            image_width,                                                       \
            image_height,                                                      \
            tile_size,                                                         \
            viewmats0,                                                         \
            viewmats1,                                                         \
            Ks,                                                                \
            camera_model,                                                     \
            ut_params,                                                        \
            rs_type,                                                       \
            radial_coeffs,                                                    \
            tangential_coeffs,                                                \
            thin_prism_coeffs,                                               \
            ftheta_coeffs,                                                     \
            tile_offsets,                                                      \
            flatten_ids,                                                       \
            render_alphas,                                                     \
            last_ids,                                                          \
            v_render_colors,                                                   \
            v_render_alphas,                                                   \
            v_means,                                                           \
            v_quats,                                                           \
            v_scales,                                                          \
            v_colors,                                                          \
            v_opacities                                                        \
        );                                                                     \
        break;

    // TODO: an optimization can be done by passing the actual number of
    // channels into the kernel functions and avoid necessary global memory
    // writes. This requires moving the channel padding from python to C side.
    switch (channels) {
        __LAUNCH_KERNEL__(1)
        __LAUNCH_KERNEL__(2)
        __LAUNCH_KERNEL__(3)
        __LAUNCH_KERNEL__(4)
        __LAUNCH_KERNEL__(5)
        __LAUNCH_KERNEL__(8)
        __LAUNCH_KERNEL__(9)
        __LAUNCH_KERNEL__(16)
        __LAUNCH_KERNEL__(17)
        __LAUNCH_KERNEL__(32)
        __LAUNCH_KERNEL__(33)
        __LAUNCH_KERNEL__(64)
        __LAUNCH_KERNEL__(65)
        __LAUNCH_KERNEL__(128)
        __LAUNCH_KERNEL__(129)
        __LAUNCH_KERNEL__(256)
        __LAUNCH_KERNEL__(257)
        __LAUNCH_KERNEL__(512)
        __LAUNCH_KERNEL__(513)
    default:
        AT_ERROR("Unsupported number of channels: ", channels);
    }
#undef __LAUNCH_KERNEL__

    return std::make_tuple(
        v_means, v_quats, v_scales, v_colors, v_opacities
    );
}

} // namespace gsplat
