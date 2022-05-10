/*
* Copyright(c) 2019 Intel Corporation
* Copyright (c) 2016, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#include <stdlib.h>
#include "aom_dsp_rtcd.h"
#include "EbDefinitions.h"
#include "EbEncHandle.h"
#include "EbCdefProcess.h"
#include "EbEncDecResults.h"
#include "EbThreads.h"
#include "EbReferenceObject.h"
#include "EbEncCdef.h"
#include "EbEncDecProcess.h"
#include "EbPictureBufferDesc.h"
#include "EbSequenceControlSet.h"
#include "EbUtility.h"
#include "EbPictureControlSet.h"

void copy_sb8_16(uint16_t *dst, int32_t dstride, const uint8_t *src, int32_t src_voffset,
                 int32_t src_hoffset, int32_t sstride, int32_t vsize, int32_t hsize, Bool is_16bit);

void   *svt_aom_memalign(size_t align, size_t size);
void    svt_aom_free(void *memblk);
void   *svt_aom_malloc(size_t size);
int32_t svt_sb_all_skip(PictureControlSet *pcs_ptr, const Av1Common *const cm, int32_t mi_row,
                        int32_t mi_col);
int32_t svt_sb_compute_cdef_list(PictureControlSet *pcs_ptr, const Av1Common *const cm,
                                 int32_t mi_row, int32_t mi_col, CdefList *dlist, BlockSize bs);
void    finish_cdef_search(PictureControlSet *pcs_ptr);
void    svt_av1_cdef_frame(SequenceControlSet *scs, PictureControlSet *pcs);
void    svt_av1_loop_restoration_save_boundary_lines(const Yv12BufferConfig *frame, Av1Common *cm,
                                                     int32_t after_cdef);
void    svt_av1_superres_upscale_frame(struct Av1Common *cm, PictureControlSet *pcs_ptr,
                                       SequenceControlSet *scs_ptr);
void    set_unscaled_input_16bit(PictureControlSet *pcs_ptr);

void get_recon_pic(PictureControlSet *pcs_ptr, EbPictureBufferDesc **recon_ptr, Bool is_highbd);

/**************************************
 * Cdef Context
 **************************************/
typedef struct CdefContext {
    EbFifo *cdef_input_fifo_ptr;
    EbFifo *cdef_output_fifo_ptr;
} CdefContext;

static void cdef_context_dctor(EbPtr p) {
    EbThreadContext *thread_context_ptr = (EbThreadContext *)p;
    CdefContext     *obj                = (CdefContext *)thread_context_ptr->priv;
    EB_FREE_ARRAY(obj);
}

/******************************************************
 * Cdef Context Constructor
 ******************************************************/
EbErrorType cdef_context_ctor(EbThreadContext   *thread_context_ptr,
                              const EbEncHandle *enc_handle_ptr, int index) {
    CdefContext *context_ptr;
    EB_CALLOC_ARRAY(context_ptr, 1);
    thread_context_ptr->priv  = context_ptr;
    thread_context_ptr->dctor = cdef_context_dctor;

    // Input/Output System Resource Manager FIFOs
    context_ptr->cdef_input_fifo_ptr = svt_system_resource_get_consumer_fifo(
        enc_handle_ptr->dlf_results_resource_ptr, index);
    context_ptr->cdef_output_fifo_ptr = svt_system_resource_get_producer_fifo(
        enc_handle_ptr->cdef_results_resource_ptr, index);

    return EB_ErrorNone;
}

#define default_mse_uv 1040400
static uint64_t compute_cdef_dist(const EbByte dst, int32_t doffset, int32_t dstride, const uint8_t *src, const CdefList *dlist,
    int32_t cdef_count, BlockSize bsize, int32_t coeff_shift, int32_t pli, uint8_t subsampling_factor, Bool is_16bit) {
    uint64_t curr_mse = 0;
    if (is_16bit) {
        curr_mse = svt_compute_cdef_dist_16bit(
            ((uint16_t*)dst) + doffset,
            dstride,
            (uint16_t*)src,
            dlist,
            cdef_count,
            bsize,
            coeff_shift,
            pli,
            subsampling_factor);

    }
    else {
        curr_mse = svt_compute_cdef_dist_8bit(
            dst + doffset,
            dstride,
            src,
            dlist,
            cdef_count,
            bsize,
            coeff_shift,
            pli,
            subsampling_factor);
    }
    return curr_mse;
}

/* Search for the best filter strength pair for each 64x64 filter block.
 *
 * For each 64x64 filter block and each plane, search the allowable filter strength pairs.
 * Call cdef_filter_fb() to perform filtering, then compute the MSE for each pair.
*/
static void cdef_seg_search(PictureControlSet *pcs, SequenceControlSet *scs,
                     uint32_t segment_index) {

    struct PictureParentControlSet *ppcs    = pcs->parent_pcs_ptr;
    FrameHeader                    *frm_hdr = &ppcs->frm_hdr;
    Av1Common                      *cm      = ppcs->av1_cm;
    const Bool                      is_16bit = scs->is_16bit_pipeline;
    uint32_t                        x_seg_idx;
    uint32_t                        y_seg_idx;
    const uint32_t b64_pic_width = (ppcs->aligned_width + 64 - 1) / 64;
    const uint32_t b64_pic_height = (ppcs->aligned_height + 64 - 1) / 64;
    SEGMENT_CONVERT_IDX_TO_XY(
        segment_index, x_seg_idx, y_seg_idx, pcs->cdef_segments_column_count);
    const uint32_t x_b64_start_idx = SEGMENT_START_IDX(
        x_seg_idx, b64_pic_width, pcs->cdef_segments_column_count);
    const uint32_t x_b64_end_idx = SEGMENT_END_IDX(
        x_seg_idx, b64_pic_width, pcs->cdef_segments_column_count);
    const uint32_t y_b64_start_idx = SEGMENT_START_IDX(
        y_seg_idx, b64_pic_height, pcs->cdef_segments_row_count);
    const uint32_t y_b64_end_idx = SEGMENT_END_IDX(
        y_seg_idx, b64_pic_height, pcs->cdef_segments_row_count);

    const int32_t mi_rows = cm->mi_rows;
    const int32_t mi_cols = cm->mi_cols;
    CdefControls *cdef_ctrls                 = &ppcs->cdef_ctrls;
    const int     first_pass_fs_num          = cdef_ctrls->first_pass_fs_num;
    const int     default_second_pass_fs_num = cdef_ctrls->default_second_pass_fs_num;
    EbByte        src[3];
    EbByte        ref[3];
    int32_t       stride_src[3];
    int32_t       stride_ref[3];
    int32_t       bsize[3];
    int32_t       mi_wide_l2[3];
    int32_t       mi_high_l2[3];
    int32_t       xdec[3];
    int32_t       ydec[3];
    int32_t       cdef_count;
    const int32_t coeff_shift = AOMMAX(scs->static_config.encoder_bit_depth - 8, 0);
    const int32_t nvfb        = (mi_rows + MI_SIZE_64X64 - 1) / MI_SIZE_64X64;
    const int32_t nhfb        = (mi_cols + MI_SIZE_64X64 - 1) / MI_SIZE_64X64;
    const int32_t pri_damping = 3 + (frm_hdr->quantization_params.base_q_idx >> 6);
    const int32_t sec_damping = pri_damping;
    const int32_t num_planes  = 3;
    CdefList      dlist[MI_SIZE_128X128 * MI_SIZE_128X128];

    DECLARE_ALIGNED(32, uint16_t, inbuf[CDEF_INBUF_SIZE]);
    uint16_t *in = inbuf + CDEF_VBORDER * CDEF_BSTRIDE + CDEF_HBORDER;
    // tmp_dst is uint16_t to accomodate high bit depth content; 8bit will treat it as a uint8_t buffer
    // and will not use half of the buffer
    DECLARE_ALIGNED(32, uint16_t, tmp_dst[1 << (MAX_SB_SIZE_LOG2 * 2)]);

    EbPictureBufferDesc *input_pic = is_16bit ?
        pcs->input_frame16bit : ppcs->enhanced_picture_ptr;
    EbPictureBufferDesc *recon_pic;
    get_recon_pic(pcs, &recon_pic, is_16bit);

    for (int pli = 0; pli < num_planes; pli++) {
        const int subsampling_x = (pli == 0) ? 0 : 1;
        const int subsampling_y = (pli == 0) ? 0 : 1;
        xdec[pli]             = subsampling_x;
        ydec[pli]             = subsampling_y;
        bsize[pli]            = subsampling_y ? (subsampling_x ? BLOCK_4X4 : BLOCK_8X4)
                                              : (subsampling_x ? BLOCK_4X8 : BLOCK_8X8);
        mi_wide_l2[pli]       = MI_SIZE_LOG2 - subsampling_x;
        mi_high_l2[pli]       = MI_SIZE_LOG2 - subsampling_y;
        src[pli] = pcs->cdef_input_recon[pli];
        ref[pli] = pcs->cdef_input_source[pli];
        stride_src[pli] = pli == 0
            ? recon_pic->stride_y
            : (pli == 1 ? recon_pic->stride_cb : recon_pic->stride_cr);
        stride_ref[pli] = pli == 0
            ? input_pic->stride_y
            : (pli == 1 ? input_pic->stride_cb : input_pic->stride_cr);
    }

    // Loop over all filter blocks (64x64)
    for (uint32_t fbr = y_b64_start_idx; fbr < y_b64_end_idx; ++fbr) {
        for (uint32_t fbc = x_b64_start_idx; fbc < x_b64_end_idx; ++fbc) {
            int32_t dirinit = 0;
            const uint32_t lc = MI_SIZE_64X64 * fbc;
            const uint32_t lr = MI_SIZE_64X64 * fbr;
            int nhb = AOMMIN(MI_SIZE_64X64, mi_cols - lc);
            int nvb = AOMMIN(MI_SIZE_64X64, mi_rows - lr);
            int hb_step = 1; //these should be all time with 64x64 SBs
            int vb_step = 1;
            BlockSize         bs      = BLOCK_64X64;
            ModeInfo        **mi      = pcs->mi_grid_base + lr * cm->mi_stride + lc;
            const MbModeInfo *mbmi    = &mi[0]->mbmi;
            const BlockSize   sb_type = mbmi->block_mi.sb_type;
            if (((fbc & 1) && (sb_type == BLOCK_128X128 || sb_type == BLOCK_128X64)) ||
                ((fbr & 1) && (sb_type == BLOCK_128X128 || sb_type == BLOCK_64X128)))
                continue;
            if (sb_type == BLOCK_128X128 || sb_type == BLOCK_128X64 || sb_type == BLOCK_64X128)
                bs = sb_type;

            if (bs == BLOCK_128X128 || bs == BLOCK_128X64) {
                nhb     = AOMMIN(MI_SIZE_128X128, cm->mi_cols - lc);
                hb_step = 2;
            }
            if (bs == BLOCK_128X128 || bs == BLOCK_64X128) {
                nvb     = AOMMIN(MI_SIZE_128X128, cm->mi_rows - lr);
                vb_step = 2;
            }
            const uint32_t fb_idx = fbr * nhfb + fbc;
            // No filtering if the entire filter block is skipped
            cdef_count = svt_sb_compute_cdef_list(pcs, cm, lr, lc, dlist, bs);
            if (cdef_count == 0) {
                pcs->skip_cdef_seg[fb_idx] = 1;
                continue;
            }
            pcs->skip_cdef_seg[fb_idx] = 0;

            uint8_t(*dir)[CDEF_NBLOCKS][CDEF_NBLOCKS] = &pcs->cdef_dir_data[fb_idx].dir;
            int32_t(*var)[CDEF_NBLOCKS][CDEF_NBLOCKS] = &pcs->cdef_dir_data[fb_idx].var;
            for (int pli = 0; pli < num_planes; pli++) {
                /* We avoid filtering the pixels for which some of the pixels to
                   average are outside the frame. We could change the filter instead,
                   but it would add special cases for any future vectorization.
                   No need to set pli == 2 because the copy size will be the same as for pli == 1. */
                if (pli < 2)
                    memset(inbuf, (uint8_t)CDEF_VERY_LARGE, sizeof(inbuf[0]) * CDEF_INBUF_SIZE);
                int32_t yoff  = CDEF_VBORDER * (fbr != 0);
                int32_t xoff  = CDEF_HBORDER * (fbc != 0);
                int32_t ysize = (nvb << mi_high_l2[pli]) +
                    CDEF_VBORDER * ((int32_t)fbr + vb_step < nvfb) + yoff;
                int32_t xsize = (nhb << mi_wide_l2[pli]) +
                    CDEF_HBORDER * ((int32_t)fbc + hb_step < nhfb) + xoff;

                copy_sb8_16(&in[(-yoff * CDEF_BSTRIDE - xoff)],
                            CDEF_BSTRIDE,
                            src[pli],
                            (lr << mi_high_l2[pli]) - yoff,
                            (lc << mi_wide_l2[pli]) - xoff,
                            stride_src[pli],
                            ysize,
                            xsize,
                            is_16bit);

                uint8_t subsampling_factor = cdef_ctrls->subsampling_factor;
                /*
                Cap the subsampling for certain block sizes.

                The intrinsics process several lines simultaneously, so blocks can only be subsampled
                a finite amount before there is no more speed gain.  If the space between processed lines
                is too large, the intrinsics will begin accessing memory outside the block.
                */
                switch (bsize[pli]) {
                case BLOCK_8X8:
                    subsampling_factor = MIN(subsampling_factor, 4);
                    break;
                case BLOCK_8X4:
                case BLOCK_4X8:
                    subsampling_factor = MIN(subsampling_factor, 2);
                    break;
                case BLOCK_4X4:
                    subsampling_factor = MIN(subsampling_factor, 1);
                    break;
                }

                /* first cdef stage
                 * Perform the pri_filter strength search for the current sub_block
                 */
                for (int gi = 0; gi < first_pass_fs_num; gi++) {

                    // Check if chroma filter is set to be tested
                    if (pli && (cdef_ctrls->default_first_pass_fs_uv[gi] == -1)) {
                        pcs->mse_seg[1][fb_idx][gi] = default_mse_uv * 64;
                        continue;
                    }

                    int32_t pri_strength = cdef_ctrls->default_first_pass_fs[gi] / CDEF_SEC_STRENGTHS;
                    int32_t sec_strength = cdef_ctrls->default_first_pass_fs[gi] % CDEF_SEC_STRENGTHS;

                    svt_cdef_filter_fb(is_16bit ? NULL : (uint8_t*)tmp_dst,
                                        is_16bit ? tmp_dst : NULL,
                                        0,
                                        in,
                                        xdec[pli],
                                        ydec[pli],
                                        *dir,
                                        &dirinit,
                                        *var,
                                        pli,
                                        dlist,
                                        cdef_count,
                                        pri_strength,
                                        sec_strength + (sec_strength == 3),
                                        pri_damping,
                                        sec_damping,
                                        coeff_shift,
                                        subsampling_factor);
                    uint64_t curr_mse = compute_cdef_dist(
                        ref[pli],
                        (lr << mi_high_l2[pli]) * stride_ref[pli] + (lc << mi_wide_l2[pli]),
                        stride_ref[pli],
                        (uint8_t*)tmp_dst,
                        dlist,
                        cdef_count,
                        (BlockSize)bsize[pli],
                        coeff_shift,
                        pli,
                        subsampling_factor,
                        is_16bit);

                    if (pli < 2)
                        pcs->mse_seg[pli][fb_idx][gi] = curr_mse * subsampling_factor;
                    else
                        pcs->mse_seg[1][fb_idx][gi] += (curr_mse * subsampling_factor);
                }

                /* second cdef stage
                 * Perform the sec_filter strength search for the current sub_block
                 */
                for (int gi = first_pass_fs_num; gi < first_pass_fs_num + default_second_pass_fs_num;
                     gi++) {

                    // Check if chroma filter is set to be tested
                    if (pli && (cdef_ctrls->default_second_pass_fs_uv[gi - first_pass_fs_num] == -1)) {
                        pcs->mse_seg[1][fb_idx][gi] = default_mse_uv * 64;
                        continue;
                    }

                    int32_t pri_strength = cdef_ctrls->default_second_pass_fs[gi - first_pass_fs_num] /
                        CDEF_SEC_STRENGTHS;
                    int32_t sec_strength = cdef_ctrls->default_second_pass_fs[gi - first_pass_fs_num] %
                        CDEF_SEC_STRENGTHS;

                    svt_cdef_filter_fb(is_16bit ? NULL : (uint8_t*)tmp_dst,
                                        is_16bit ? tmp_dst : NULL,
                                        0,
                                        in,
                                        xdec[pli],
                                        ydec[pli],
                                        *dir,
                                        &dirinit,
                                        *var,
                                        pli,
                                        dlist,
                                        cdef_count,
                                        pri_strength,
                                        sec_strength + (sec_strength == 3),
                                        pri_damping,
                                        sec_damping,
                                        coeff_shift,
                                        subsampling_factor);
                    uint64_t curr_mse = compute_cdef_dist(
                        ref[pli],
                        (lr << mi_high_l2[pli]) * stride_ref[pli] + (lc << mi_wide_l2[pli]),
                        stride_ref[pli],
                        (uint8_t*)tmp_dst,
                        dlist,
                        cdef_count,
                        (BlockSize)bsize[pli],
                        coeff_shift,
                        pli,
                        subsampling_factor,
                        is_16bit);

                    if (pli < 2)
                        pcs->mse_seg[pli][fb_idx][gi] = curr_mse * subsampling_factor;
                    else
                        pcs->mse_seg[1][fb_idx][gi] += (curr_mse * subsampling_factor);
                }
            }
        }
    }
}

/******************************************************
 * CDEF Kernel
 ******************************************************/
void *cdef_kernel(void *input_ptr) {
    // Context & SCS & PCS
    EbThreadContext    *thread_context_ptr = (EbThreadContext *)input_ptr;
    CdefContext        *context_ptr        = (CdefContext *)thread_context_ptr->priv;
    PictureControlSet  *pcs_ptr;
    SequenceControlSet *scs_ptr;

    //// Input
    EbObjectWrapper *dlf_results_wrapper_ptr;
    DlfResults      *dlf_results_ptr;

    //// Output
    EbObjectWrapper *cdef_results_wrapper_ptr;
    CdefResults     *cdef_results_ptr;

    // SB Loop variables

    for (;;) {
        FrameHeader *frm_hdr;

        // Get DLF Results
        EB_GET_FULL_OBJECT(context_ptr->cdef_input_fifo_ptr, &dlf_results_wrapper_ptr);

        dlf_results_ptr = (DlfResults *)dlf_results_wrapper_ptr->object_ptr;
        pcs_ptr         = (PictureControlSet *)dlf_results_ptr->pcs_wrapper_ptr->object_ptr;
        PictureParentControlSet* ppcs = pcs_ptr->parent_pcs_ptr;
        scs_ptr         = pcs_ptr->scs_ptr;

        Bool       is_16bit      = scs_ptr->is_16bit_pipeline;
        Av1Common *cm            = pcs_ptr->parent_pcs_ptr->av1_cm;
        frm_hdr                  = &pcs_ptr->parent_pcs_ptr->frm_hdr;
        CdefControls *cdef_ctrls = &pcs_ptr->parent_pcs_ptr->cdef_ctrls;
        if (!cdef_ctrls->use_reference_cdef_fs) {
            if (scs_ptr->seq_header.cdef_level && pcs_ptr->parent_pcs_ptr->cdef_level) {
                cdef_seg_search(pcs_ptr, scs_ptr, dlf_results_ptr->segment_index);
            }
        }
        //all seg based search is done. update total processed segments. if all done, finish the search and perfrom application.
        svt_block_on_mutex(pcs_ptr->cdef_search_mutex);

        pcs_ptr->tot_seg_searched_cdef++;
        if (pcs_ptr->tot_seg_searched_cdef == pcs_ptr->cdef_segments_total_count) {
            // SVT_LOG("    CDEF all seg here  %i\n", pcs_ptr->picture_number);
            if (scs_ptr->seq_header.cdef_level && pcs_ptr->parent_pcs_ptr->cdef_level) {
                finish_cdef_search(pcs_ptr);
                if (ppcs->enable_restoration ||
                    pcs_ptr->parent_pcs_ptr->is_used_as_reference_flag ||
                    scs_ptr->static_config.recon_enabled) {
                    // Do application iff there are non-zero filters
                    if (frm_hdr->cdef_params.cdef_y_strength[0] != 0 ||
                        frm_hdr->cdef_params.cdef_uv_strength[0] != 0 ||
                        pcs_ptr->parent_pcs_ptr->nb_cdef_strengths != 1) {
                        svt_av1_cdef_frame(scs_ptr, pcs_ptr);
                    }
                }
            } else {
                frm_hdr->cdef_params.cdef_bits             = 0;
                frm_hdr->cdef_params.cdef_y_strength[0]    = 0;
                pcs_ptr->parent_pcs_ptr->nb_cdef_strengths = 1;
                frm_hdr->cdef_params.cdef_uv_strength[0]   = 0;
            }

            //restoration prep
            if (ppcs->enable_restoration) {
                svt_av1_loop_restoration_save_boundary_lines(cm->frame_to_show, cm, 1);
            }

            // ------- start: Normative upscaling - super-resolution tool
            if (frm_hdr->allow_intrabc == 0 && pcs_ptr->parent_pcs_ptr->frame_superres_enabled) {
                svt_av1_superres_upscale_frame(cm, pcs_ptr, scs_ptr);

                if (is_16bit) {
                    set_unscaled_input_16bit(pcs_ptr);
                }
            }
            if (scs_ptr->static_config.resize_mode != RESIZE_NONE) {
                EbPictureBufferDesc* recon = NULL;
                get_recon_pic(pcs_ptr, &recon, is_16bit);
                recon->width = pcs_ptr->parent_pcs_ptr->render_width;
                recon->height = pcs_ptr->parent_pcs_ptr->render_height;;
                //TODO: 16-bit
            }
            // ------- end: Normative upscaling - super-resolution tool

            pcs_ptr->rest_segments_column_count = scs_ptr->rest_segment_column_count;
            pcs_ptr->rest_segments_row_count    = scs_ptr->rest_segment_row_count;
            pcs_ptr->rest_segments_total_count  = (uint16_t)(pcs_ptr->rest_segments_column_count *
                                                            pcs_ptr->rest_segments_row_count);
            pcs_ptr->tot_seg_searched_rest      = 0;
            pcs_ptr->parent_pcs_ptr->av1_cm->use_boundaries_in_rest_search =
                scs_ptr->use_boundaries_in_rest_search;
            pcs_ptr->rest_extend_flag[0] = FALSE;
            pcs_ptr->rest_extend_flag[1] = FALSE;
            pcs_ptr->rest_extend_flag[2] = FALSE;

            uint32_t segment_index;
            for (segment_index = 0; segment_index < pcs_ptr->rest_segments_total_count;
                 ++segment_index) {
                // Get Empty Cdef Results to Rest
                svt_get_empty_object(context_ptr->cdef_output_fifo_ptr, &cdef_results_wrapper_ptr);
                cdef_results_ptr = (struct CdefResults *)cdef_results_wrapper_ptr->object_ptr;
                cdef_results_ptr->pcs_wrapper_ptr = dlf_results_ptr->pcs_wrapper_ptr;
                cdef_results_ptr->segment_index   = segment_index;
                // Post Cdef Results
                svt_post_full_object(cdef_results_wrapper_ptr);
            }
        }
        svt_release_mutex(pcs_ptr->cdef_search_mutex);

        // Release Dlf Results
        svt_release_object(dlf_results_wrapper_ptr);
    }

    return NULL;
}
