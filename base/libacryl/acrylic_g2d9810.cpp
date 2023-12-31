/*
 * Copyright Samsung Electronics Co.,LTD.
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstring>
#include <alloca.h>
#include <algorithm>

#include <sys/ioctl.h>

#include <system/graphics.h>
#include <log/log.h>

#include <hardware/hwcomposer2.h>

#include <exynos_format.h> // hardware/smasung_slsi/exynos/include

#include "acrylic_g2d9810.h"

enum {
    G2D_CSC_STD_UNDEFINED = -1,
    G2D_CSC_STD_601       = 0,
    G2D_CSC_STD_709       = 1,
    G2D_CSC_STD_2020      = 2,
    G2D_CSC_STD_P3        = 3,

    G2D_CSC_STD_COUNT     = 4,
};

enum {
    G2D_CSC_RANGE_LIMITED,
    G2D_CSC_RANGE_FULL,

    G2D_CSC_RANGE_COUNT,
};

static char csc_std_to_matrix_index[] = {
    G2D_CSC_STD_709,                          // HAL_DATASPACE_STANDARD_UNSPECIFIED
    G2D_CSC_STD_709,                          // HAL_DATASPACE_STANDARD_BT709
    G2D_CSC_STD_601,                          // HAL_DATASPACE_STANDARD_BT601_625
    G2D_CSC_STD_601,                          // HAL_DATASPACE_STANDARD_BT601_625_UNADJUSTED
    G2D_CSC_STD_601,                          // HAL_DATASPACE_STANDARD_BT601_525
    G2D_CSC_STD_601,                          // HAL_DATASPACE_STANDARD_BT601_525_UNADJUSTED
    G2D_CSC_STD_2020,                         // HAL_DATASPACE_STANDARD_BT2020
    G2D_CSC_STD_2020,                         // HAL_DATASPACE_STANDARD_BT2020_CONSTANT_LUMINANCE
    static_cast<char>(G2D_CSC_STD_UNDEFINED), // HAL_DATASPACE_STANDARD_BT470M
    G2D_CSC_STD_709,                          // HAL_DATASPACE_STANDARD_FILM
    G2D_CSC_STD_P3,                           // HAL_DATASPACE_STANDARD_DCI_P3
    static_cast<char>(G2D_CSC_STD_UNDEFINED), // HAL_DATASPACE_STANDARD_ADOBE_RGB
};

static uint16_t YCbCr2sRGBCoefficients[G2D_CSC_STD_COUNT * G2D_CSC_RANGE_COUNT][9] = {
    {0x0254, 0x0000, 0x0331, 0x0254, 0xFF37, 0xFE60, 0x0254, 0x0409, 0x0000}, // 601 limited
    {0x0200, 0x0000, 0x02BE, 0x0200, 0xFF54, 0xFE9B, 0x0200, 0x0377, 0x0000}, // 601 full
    {0x0254, 0x0000, 0x0396, 0x0254, 0xFF93, 0xFEEF, 0x0254, 0x043A, 0x0000}, // 709 limited
    {0x0200, 0x0000, 0x0314, 0x0200, 0xFFA2, 0xFF16, 0x0200, 0x03A1, 0x0000}, // 709 full
    {0x0254, 0x0000, 0x035B, 0x0254, 0xFFA0, 0xFEB3, 0x0254, 0x0449, 0x0000}, // 2020 limited
    {0x0200, 0x0000, 0x02E2, 0x0200, 0xFFAE, 0xFEE2, 0x0200, 0x03AE, 0x0000}, // 2020 full
    {0x0254, 0x0000, 0x03AE, 0x0254, 0xFF96, 0xFEEE, 0x0254, 0x0456, 0x0000}, // DCI-P3 limited
    {0x0200, 0x0000, 0x0329, 0x0200, 0xFFA5, 0xFF15, 0x0200, 0x03B9, 0x0000}, // DCI-P3 full
};

static uint16_t sRGB2YCbCrCoefficients[G2D_CSC_STD_COUNT * G2D_CSC_RANGE_COUNT][9] = {
    {0x0083, 0x0102, 0x0032, 0xFFB4, 0xFF6B, 0x00E1, 0x00E1, 0xFF44, 0xFFDB}, // 601 limited
    {0x0099, 0x012D, 0x003A, 0xFFA8, 0xFF53, 0x0106, 0x0106, 0xFF25, 0xFFD5}, // 601 full
    {0x005D, 0x013A, 0x0020, 0xFFCC, 0xFF53, 0x00E1, 0x00E1, 0xFF34, 0xFFEB}, // 709 limited
    {0x006D, 0x016E, 0x0025, 0xFFC4, 0xFF36, 0x0106, 0x0106, 0xFF12, 0xFFE8}, // 709 full
    {0x0074, 0x012A, 0x001A, 0xFFC1, 0xFF5A, 0x00E1, 0x00E1, 0xFF31, 0xFFEE}, // 2020 limited
    {0x0087, 0x015B, 0x001E, 0xFFB7, 0xFF43, 0x0106, 0x0106, 0xFF0F, 0xFFEB}, // 2020 full
    {0x006B, 0x0171, 0x0023, 0xFFC6, 0xFF3A, 0x0100, 0x0100, 0xFF16, 0xFFEA}, // DCI-P3 limited(full)
    {0x006B, 0x0171, 0x0023, 0xFFC6, 0xFF3A, 0x0100, 0x0100, 0xFF16, 0xFFEA}, // DCI-P3 full
};

#define CSC_MATRIX_REGISTER_COUNT 9
#define CSC_MATRIX_REGISTER_SIZE  (CSC_MATRIX_REGISTER_COUNT * sizeof(uint32_t))

static inline bool g2dfmt_is_ycbcr(unsigned int g2dfmt)
{
    g2dfmt &= G2D_DATAFMT_MASK;
    return (G2D_DATAFMT_YUV_MIN <= g2dfmt) && (g2dfmt <= G2D_DATAFMT_YUV_MAX);
}

class CSCMatrixWriter {
    enum { CSC_MATRIX_MAX_COUNT = 4, CSC_MATRIX_INVALID_INDEX = 200 };
    enum { CSC_MATRIX_SRC_BASE = 0x2000, CSC_MATRIX_DST_BASE = 0x2100 };
public:
    CSCMatrixWriter(unsigned int g2dfmt, unsigned int dataspace, uint32_t *command)
                    : mMatrixCount(0), mMatrixTargetIndex(CSC_MATRIX_INVALID_INDEX) {
        // Ignore if unsupported dataspace is specified.
        // G2D also works for the case.
        // But the correctness of the result is not guaranteed.
        if (g2dfmt_is_ycbcr(g2dfmt)) {
            mMatrixTargetIndex = findMatrixIndex(dataspace);
            if ((dataspace & HAL_DATASPACE_RANGE_FULL) != 0)
                *command = G2D_LAYER_YCBCRMODE_WIDE;
        }
    }

    bool configure(unsigned int g2dfmt, unsigned int dataspace, uint32_t *command) {
        if (!g2dfmt_is_ycbcr(g2dfmt))
            return true;

        unsigned int spcidx = findMatrixIndex(dataspace);

        if (spcidx == CSC_MATRIX_INVALID_INDEX)
            return false;

        if ((dataspace & HAL_DATASPACE_RANGE_FULL) != 0)
            *command = G2D_LAYER_YCBCRMODE_WIDE;

        for (int i = 0; i < mMatrixCount; i++) {
            if (mMatrixIndex[i] == spcidx) {
                *command |= i;
                return true;
            }
        }

        if (mMatrixCount == CSC_MATRIX_MAX_COUNT) {
            ALOGE("Too many CSC requirements");
            return false;
        }

        mMatrixIndex[mMatrixCount] = spcidx;

        *command |= mMatrixCount++;
        // 8-bit part data are always dithered by MFC
        if ((g2dfmt & G2D_DATAFMT_YUV420SP82_9810) != 0)
            *command |= G2D_YCBCRMODE_DITHER;

        return true;
    }

    unsigned int getRegisterCount() {
        unsigned int count = CSC_MATRIX_REGISTER_COUNT * mMatrixCount;
        if (mMatrixTargetIndex != CSC_MATRIX_INVALID_INDEX)
            count += CSC_MATRIX_REGISTER_COUNT;
        return count;
    }

    unsigned int write(g2d_reg regs[]) {
        unsigned int count = 0;

        if (mMatrixTargetIndex != CSC_MATRIX_INVALID_INDEX) {
            writeSingle(CSC_MATRIX_DST_BASE,
                        &regs[count], sRGB2YCbCrCoefficients[mMatrixTargetIndex]);
            count += CSC_MATRIX_REGISTER_COUNT;
        }

        for (int m = 0; m < mMatrixCount; m++) {
            writeSingle(CSC_MATRIX_SRC_BASE + m * CSC_MATRIX_REGISTER_SIZE,
                        &regs[count], YCbCr2sRGBCoefficients[mMatrixIndex[m]]);
            count += CSC_MATRIX_REGISTER_COUNT;
        }

        return count;
    }

private:
    void writeSingle(unsigned int base, g2d_reg regs[], uint16_t matrix[9]) {
        for (unsigned int idx = 0; idx < CSC_MATRIX_REGISTER_COUNT; idx++) {
            regs[idx].offset = base;
            regs[idx].value = matrix[idx];
            base += sizeof(uint32_t);
        }
    }

    unsigned int findMatrixIndex(unsigned int dataspace) {
        unsigned int index, colorspace;

        colorspace = (dataspace & HAL_DATASPACE_STANDARD_MASK) >> HAL_DATASPACE_STANDARD_SHIFT;
        if (colorspace >= ARRSIZE(csc_std_to_matrix_index)) {
            ALOGE("Data space %d is not supported by G2D", dataspace);
            return CSC_MATRIX_INVALID_INDEX;
        }

        index = csc_std_to_matrix_index[colorspace] * G2D_CSC_RANGE_COUNT;
        if ((dataspace & HAL_DATASPACE_RANGE_FULL) != 0)
            index++;

        return index;
    }

    unsigned int mMatrixIndex[CSC_MATRIX_MAX_COUNT];
    int mMatrixCount;
    unsigned int mMatrixTargetIndex;
};

static void show_g2d_layer(const char *title, int idx, const g2d_layer &layer)
{
    ALOGD("%s%d: flags %#x, fence %d, buffer_type %d, num_buffers %d", title, idx,
          layer.flags, layer.fence, layer.buffer_type, layer.num_buffers);
    for (unsigned int i = 0; i < layer.num_buffers; i++) {
        ALOGD("         buf[%d] ptr %p, fd %d, offset %u, length %u",
              i, layer.buffer[i].userptr,
              layer.buffer[i].dmabuf.fd, layer.buffer[i].dmabuf.offset,
              layer.buffer[i].length);
    }
}

static void show_g2d_commands(const g2d_commands &cmds)
{
    for (unsigned int i = 0; i < G2DSFR_DST_FIELD_COUNT; i++)
        ALOGD("DST[%02d]: %#010x", i, cmds.target[i]);

    for (unsigned int idx = 0; idx < G2D_MAX_IMAGES; idx++) {
        if (cmds.source[idx]) {
            for (unsigned int i = 0; i < G2DSFR_SRC_FIELD_COUNT; i++)
                ALOGD("SRC[%02d][%02d]: %#010x", idx, i, cmds.source[idx][i]);
        }
    }

    if (cmds.extra) {
        for (unsigned int i = 0; i < cmds.num_extra_regs; i++)
            ALOGD("EXTRA: offset %#010x, value %#010x",
                  cmds.extra[i].offset, cmds.extra[i].value);
    }
}

static void show_g2d_task(const g2d_task &task)
{
    ALOGD("Showing the content of G2D task descriptor ver %#010x", task.version);
    ALOGD("source count %d, flags %#x, priority %d, num_release_fences %d",
          task.num_source, task.flags, task.priority, task.num_release_fences);
    show_g2d_layer("Target", 0, task.target);
    for (unsigned int i = 0; i < task.num_source; i++)
        show_g2d_layer("Source", i, task.source[i]);
    show_g2d_commands(task.commands);
}

#ifdef LIBACRYL_DEBUG
static void debug_show_g2d_task(const g2d_task &task)
{
    ALOGD("Showing the content of G2D task descriptor ver %#010x", task.version);
    ALOGD("source count %d, flags %#x, priority %d, num_release_fences %d",
          task.num_source, task.flags, task.priority, task.num_release_fences);
    show_g2d_layer("Target", 0, task.target);
    for (unsigned int i = 0; i < task.num_source; i++)
        show_g2d_layer("Source", i, task.source[i]);
    show_g2d_commands(task.commands);
}
#else
#define debug_show_g2d_task(task) do { } while (0)
#endif

struct g2d_fmt {
    uint32_t halfmt;
    uint32_t g2dfmt;
    uint32_t num_bufs;
    uint32_t rgb_bpp;
};

static g2d_fmt __halfmt_to_g2dfmt_9810[] = {
//  {halfmt,                                      g2dfmt,  num_buffers, rgbbpp}
    {HAL_PIXEL_FORMAT_RGBA_8888,                  G2D_FMT_ABGR8888,  1, 4},
    {HAL_PIXEL_FORMAT_BGRA_8888,                  G2D_FMT_ARGB8888,  1, 4},
    {HAL_PIXEL_FORMAT_RGBX_8888,                  G2D_FMT_XBGR8888,  1, 4},
    {HAL_PIXEL_FORMAT_RGBA_1010102,               G2D_FMT_ABGR2101010, 1, 4},
    {HAL_PIXEL_FORMAT_RGB_888,                    G2D_FMT_RGB888,    1, 3},
    {HAL_PIXEL_FORMAT_RGB_565,                    G2D_FMT_RGB565,    1, 2},
    {HAL_PIXEL_FORMAT_YV12,                       G2D_FMT_YV12,      1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YV12_M,              G2D_FMT_YV12,      3, 0},
//  {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P,         X,                 1, 0},
//  {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_PN,        X,                 1, 0},
//  {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P_M,       X,                 3, 0},
    {HAL_PIXEL_FORMAT_YCrCb_420_SP,               G2D_FMT_NV21,      1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M,      G2D_FMT_NV21,      2, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_FULL, G2D_FMT_NV21,      2, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP,        G2D_FMT_NV12,      1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M,      G2D_FMT_NV12,      2, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN,       G2D_FMT_NV12,      1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B,  G2D_FMT_NV12_82_9810,   1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B, G2D_FMT_NV12_82_9810,   2, 0},
    {HAL_PIXEL_FORMAT_YCBCR_P010,                 G2D_FMT_NV12_P010_9810, 1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_P010_M,        G2D_FMT_NV12_P010_9810, 2, 0},
    {HAL_PIXEL_FORMAT_YCbCr_422_I,                G2D_FMT_YUYV,      1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCrCb_422_I,         G2D_FMT_YVYU,      1, 0},
    {HAL_PIXEL_FORMAT_YCbCr_422_SP,               G2D_FMT_NV16,      1, 0},
    // TODO: add p010
};

static g2d_fmt __halfmt_to_g2dfmt_9820[] = {
//  {halfmt,                                      g2dfmt,  num_buffers, rgbbpp}
    {HAL_PIXEL_FORMAT_RGBA_8888,                  G2D_FMT_ABGR8888,  1, 4},
    {HAL_PIXEL_FORMAT_BGRA_8888,                  G2D_FMT_ARGB8888,  1, 4},
    {HAL_PIXEL_FORMAT_RGBX_8888,                  G2D_FMT_XBGR8888,  1, 4},
    {HAL_PIXEL_FORMAT_RGBA_1010102,               G2D_FMT_ABGR2101010, 1, 4},
    {HAL_PIXEL_FORMAT_RGB_888,                    G2D_FMT_RGB888,    1, 3},
    {HAL_PIXEL_FORMAT_RGB_565,                    G2D_FMT_RGB565,    1, 2},
    {HAL_PIXEL_FORMAT_YV12,                       G2D_FMT_YV12,      1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YV12_M,              G2D_FMT_YV12,      3, 0},
//  {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P,         X,                 1, 0},
//  {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_PN,        X,                 1, 0},
//  {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P_M,       X,                 3, 0},
    {HAL_PIXEL_FORMAT_YCrCb_420_SP,               G2D_FMT_NV21,      1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M,      G2D_FMT_NV21,      2, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_FULL, G2D_FMT_NV21,      2, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP,        G2D_FMT_NV12,      1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M,      G2D_FMT_NV12,      2, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN,       G2D_FMT_NV12,      1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B,  G2D_FMT_NV12_82_9820,   1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B, G2D_FMT_NV12_82_9820,   2, 0},
    {HAL_PIXEL_FORMAT_YCBCR_P010,                 G2D_FMT_NV12_P010_9820, 1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_P010_M,        G2D_FMT_NV12_P010_9820, 2, 0},
    {HAL_PIXEL_FORMAT_YCbCr_422_I,                G2D_FMT_YUYV,      1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCrCb_422_I,         G2D_FMT_YVYU,      1, 0},
    {HAL_PIXEL_FORMAT_YCbCr_422_SP,               G2D_FMT_NV16,      1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_SBWC,     G2D_FMT_NV12_SBWC, 2, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_SBWC,      G2D_FMT_NV12_SBWC, 1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_10B_SBWC, G2D_FMT_NV12_SBWC_10B, 2, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_10B_SBWC,  G2D_FMT_NV12_SBWC_10B, 1, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_SBWC,     G2D_FMT_NV21_SBWC, 2, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_10B_SBWC, G2D_FMT_NV21_SBWC_10B, 2, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_SBWC_L50, G2D_FMT_NV12_SBWC, 2, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_10B_SBWC_L40, G2D_FMT_NV12_SBWC_10B, 2, 0},
    {HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_10B_SBWC_L80, G2D_FMT_NV12_SBWC_10B, 2, 0},
};

static g2d_fmt *halfmt_to_g2dfmt(struct g2d_fmt *tbl, size_t tbl_len, uint32_t halfmt)
{
    for (size_t i = 0 ; i < tbl_len; i++) {
        if (tbl[i].halfmt == halfmt)
            return &tbl[i];
    }

    ALOGE("Unable to find the proper G2D format for HAL format %#x", halfmt);

    return NULL;
}

AcrylicCompositorG2D9810::AcrylicCompositorG2D9810(const HW2DCapability &capability, bool newcolormode)
    : Acrylic(capability), mDev((capability.maxLayerCount() > 2) ? "/dev/g2d" : "/dev/fimg2d"),
      mMaxSourceCount(0), mPriority(-1)
{
    memset(&mTask, 0, sizeof(mTask));

    mVersion = 0;
    if (mDev.ioctl(G2D_IOC_VERSION, &mVersion) < 0)
        ALOGERR("Failed to get G2D command version");
    ALOGI("G2D API Version %d", mVersion);

    halfmt_to_g2dfmt_tbl = newcolormode ? __halfmt_to_g2dfmt_9820 : __halfmt_to_g2dfmt_9810;
    len_halfmt_to_g2dfmt_tbl = newcolormode ? ARRSIZE(__halfmt_to_g2dfmt_9820) : ARRSIZE(__halfmt_to_g2dfmt_9810);

    clearLibHdrCoefficient();

    ALOGD_TEST("Created a new Acrylic for G2D 9810 on %p", this);
}

AcrylicCompositorG2D9810::~AcrylicCompositorG2D9810()
{
    delete [] mTask.source;
    delete [] mTask.commands.target;
    for (unsigned int i = 0; i < mMaxSourceCount; i++)
        delete [] mTask.commands.source[i];

    ALOGD_TEST("Deleting Acrylic for G2D 9810 on %p", this);
}

#define SBWC_BLOCK_WIDTH 32
#define SBWC_BLOCK_HEIGHT 4
#define SBWC_BLOCK_SIZE(bit) (SBWC_BLOCK_WIDTH * SBWC_BLOCK_HEIGHT * (bit) / 8)

#define SBWC_HEADER_ALIGN 16
#define SBWC_PAYLOAD_ALIGN 32

#define SBWC_HEADER_STRIDE(w) \
	ALIGN(((w) / SBWC_BLOCK_WIDTH / 2), SBWC_HEADER_ALIGN)
#define SBWC_PAYLOAD_STRIDE(w, dep)\
	ALIGN(((w) / SBWC_BLOCK_WIDTH) * SBWC_BLOCK_SIZE(dep), \
	      SBWC_PAYLOAD_ALIGN)

#define SBWC_LOSSY_PAYLOAD_STRIDE(w, block_byte) \
	ALIGN(((w) / SBWC_BLOCK_WIDTH) * (block_byte), \
	      SBWC_PAYLOAD_ALIGN)

static uint32_t mfc_stride_formats[] = {
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_SBWC,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_SBWC,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_10B_SBWC,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_10B_SBWC,
    HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_SBWC,
    HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_10B_SBWC,
};

static unsigned int sbwc_lossy_formats[] = {
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_SBWC_L50,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_10B_SBWC_L40,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_10B_SBWC_L80,
};

bool AcrylicCompositorG2D9810::prepareImage(AcrylicCanvas &layer, struct g2d_layer &image, uint32_t cmd[], int index)
{
    image.flags = 0;

    if (layer.getFence() >= 0) {
        image.flags |= G2D_LAYERFLAG_ACQUIRE_FENCE;
        image.fence = layer.getFence();
    }

    if (layer.isProtected())
        image.flags |= G2D_LAYERFLAG_SECURE;

    g2d_fmt *g2dfmt = halfmt_to_g2dfmt(halfmt_to_g2dfmt_tbl, len_halfmt_to_g2dfmt_tbl, layer.getFormat());
    if (!g2dfmt)
        return false;

    image.flags &= ~G2D_LAYERFLAG_MFC_STRIDE;
    for (size_t i = 0; i < ARRSIZE(mfc_stride_formats); i++) {
        if (layer.getFormat() == mfc_stride_formats[i]) {
            image.flags |= G2D_LAYERFLAG_MFC_STRIDE;
            break;
        }
    }

    if (layer.getBufferType() == AcrylicCanvas::MT_EMPTY) {
        image.buffer_type = G2D_BUFTYPE_EMPTY;
    } else {
        if (layer.getBufferCount() < g2dfmt->num_bufs) {
            ALOGE("HAL Format %#x requires %d buffers but %d buffers are given",
                    layer.getFormat(), g2dfmt->num_bufs, layer.getBufferCount());
            return false;
        }

        if (layer.getBufferType() == AcrylicCanvas::MT_DMABUF) {
            image.buffer_type = G2D_BUFTYPE_DMABUF;
            for (unsigned int i = 0; i < g2dfmt->num_bufs; i++) {
                image.buffer[i].dmabuf.fd = layer.getDmabuf(i);
                image.buffer[i].dmabuf.offset = layer.getOffset(i);
                image.buffer[i].length = layer.getBufferLength(i);
            }
        } else {
            LOGASSERT(layer.getBufferType() == AcrylicCanvas::MT_USERPTR,
                      "Unknown buffer type %d", layer.getBufferType());
            image.buffer_type = G2D_BUFTYPE_USERPTR;
            for (unsigned int i = 0; i < g2dfmt->num_bufs; i++) {
                image.buffer[i].userptr = layer.getUserptr(i);
                image.buffer[i].length = layer.getBufferLength(i);
            }
        }
    }

    image.num_buffers = g2dfmt->num_bufs;

    hw2d_coord_t xy = layer.getImageDimension();

    cmd[G2DSFR_IMG_COLORMODE] = g2dfmt->g2dfmt;
    if (layer.isUOrder())
        cmd[G2DSFR_IMG_COLORMODE] |= G2D_DATAFORMAT_UORDER;

    if (layer.isCompressed()) {
        // AFBC forces RGB swizzling order to BGR for RGB565
        if (g2dfmt->g2dfmt == G2D_FMT_RGB565)
            cmd[G2DSFR_IMG_COLORMODE] = G2D_FMT_BGR565;
        cmd[G2DSFR_IMG_COLORMODE] |= G2D_DATAFORMAT_AFBC;
        cmd[G2DSFR_IMG_STRIDE] = 0;
    } else if (g2dfmt->g2dfmt & G2D_DATAFORMAT_SBWC) {
        cmd[G2DSFR_IMG_STRIDE] = 0;
    } else {
        cmd[G2DSFR_IMG_STRIDE] = g2dfmt->rgb_bpp * xy.hori;
    }

    unsigned int payload = 0, header = 0, lossyByteNum = 0;

    if (g2dfmt->g2dfmt & G2D_DATAFORMAT_SBWC) {
        unsigned int blocksize;
        unsigned int isLossy = 0;
        unsigned int format = layer.getFormat();

        for (unsigned int i = 0; i < ARRSIZE(sbwc_lossy_formats); i++) {
            if (format == sbwc_lossy_formats[i]) {
                isLossy = 1;
                blocksize = (i < 2) ? 64 : 128;
                break;
            }
        }

        if (isLossy) {
            lossyByteNum = (blocksize >> 1) | isLossy;
            payload = SBWC_LOSSY_PAYLOAD_STRIDE(xy.hori, blocksize);
        } else {
            payload = SBWC_PAYLOAD_STRIDE(xy.hori, (g2dfmt->g2dfmt & G2D_FMT_YCBCR_10BIT) ? 10 : 8);
            header = SBWC_HEADER_STRIDE(xy.hori);
        }
    }

    if (index < 0) {
        cmd[G2DSFR_DST_Y_HEADER_STRIDE] = header;
        cmd[G2DSFR_DST_C_HEADER_STRIDE] = header;
        cmd[G2DSFR_DST_Y_PAYLOAD_STRIDE] = payload;
        cmd[G2DSFR_DST_C_PAYLOAD_STRIDE] = payload;
        cmd[G2DSFR_DST_SBWCINFO] = lossyByteNum;
    } else {
        cmd[G2DSFR_SRC_Y_HEADER_STRIDE] = header;
        cmd[G2DSFR_SRC_C_HEADER_STRIDE] = header;
        cmd[G2DSFR_SRC_Y_PAYLOAD_STRIDE] = payload;
        cmd[G2DSFR_SRC_C_PAYLOAD_STRIDE] = payload;
        cmd[G2DSFR_SRC_SBWCINFO] = lossyByteNum;
    }

    cmd[G2DSFR_IMG_LEFT]   = 0;
    cmd[G2DSFR_IMG_TOP]    = 0;
    cmd[G2DSFR_IMG_RIGHT]  = xy.hori;
    cmd[G2DSFR_IMG_BOTTOM] = xy.vert;
    cmd[G2DSFR_IMG_WIDTH]  = xy.hori;
    cmd[G2DSFR_IMG_HEIGHT] = xy.vert;

    return true;
}

#define G2D_SCALE_FACTOR(from, to) ((static_cast<uint32_t>(from) << G2D_SCALEFACTOR_FRACBITS) / (to))

static void setSolidLayer(struct g2d_layer &image, uint32_t cmd[], hw2d_coord_t xy)
{
    image.flags = G2D_LAYERFLAG_COLORFILL;
    image.buffer_type = G2D_BUFTYPE_EMPTY;
    image.num_buffers = 0;

    cmd[G2DSFR_IMG_COLORMODE] = G2D_FMT_ARGB8888;
    cmd[G2DSFR_IMG_STRIDE] = 4 * xy.hori;

    cmd[G2DSFR_IMG_WIDTH]  = xy.hori;
    cmd[G2DSFR_IMG_HEIGHT] = xy.vert;

    cmd[G2DSFR_SRC_SELECT] = G2D_LAYERSEL_COLORFILL;
    cmd[G2DSFR_SRC_COMMAND] = G2D_LAYERCMD_VALID;

    cmd[G2DSFR_SRC_ROTATE] = 0;
    cmd[G2DSFR_SRC_SCALECONTROL] = 0;
    cmd[G2DSFR_SRC_XSCALE] = G2D_SCALE_FACTOR(1, 1);
    cmd[G2DSFR_SRC_YSCALE] = G2D_SCALE_FACTOR(1, 1);
    cmd[G2DSFR_SRC_XPHASE] = 0;
    cmd[G2DSFR_SRC_YPHASE] = 0;
    cmd[G2DSFR_SRC_YCBCRMODE] = 0;
    cmd[G2DSFR_SRC_HDRMODE] = 0;
    cmd[G2DSFR_SRC_Y_HEADER_STRIDE] = 0;
    cmd[G2DSFR_SRC_C_HEADER_STRIDE] = 0;
    cmd[G2DSFR_SRC_Y_PAYLOAD_STRIDE] = 0;
    cmd[G2DSFR_SRC_C_PAYLOAD_STRIDE] = 0;
    cmd[G2DSFR_SRC_SBWCINFO] = 0;
}

bool AcrylicCompositorG2D9810::prepareSolidLayer(AcrylicCanvas &canvas, struct g2d_layer &image, uint32_t cmd[])
{
    hw2d_coord_t xy = canvas.getImageDimension();

    setSolidLayer(image, cmd, xy);

    uint16_t a, r, g, b;
    getBackgroundColor(&r, &g, &b, &a);

    cmd[G2DSFR_SRC_COLOR]  = (a & 0xFF00) << 16;
    cmd[G2DSFR_SRC_COLOR] |= (r & 0xFF00) << 8;
    cmd[G2DSFR_SRC_COLOR] |= (g & 0xFF00) << 0;
    cmd[G2DSFR_SRC_COLOR] |= (b & 0xFF00) >> 8;

    cmd[G2DSFR_IMG_LEFT]   = 0;
    cmd[G2DSFR_IMG_TOP]    = 0;
    cmd[G2DSFR_IMG_RIGHT]  = xy.hori;
    cmd[G2DSFR_IMG_BOTTOM] = xy.vert;

    cmd[G2DSFR_SRC_DSTLEFT]   = 0;
    cmd[G2DSFR_SRC_DSTTOP]    = 0;
    cmd[G2DSFR_SRC_DSTRIGHT]  = xy.hori;
    cmd[G2DSFR_SRC_DSTBOTTOM] = xy.vert;

    cmd[G2DSFR_SRC_ALPHA] = 0;
    cmd[G2DSFR_SRC_BLEND] = 0;

    return true;
}

bool AcrylicCompositorG2D9810::prepareSolidLayer(AcrylicLayer &layer, struct g2d_layer &image, uint32_t cmd[], hw2d_coord_t target_size, int index)
{
    hw2d_coord_t xy = layer.getImageDimension();

    setSolidLayer(image, cmd, xy);

    cmd[G2DSFR_SRC_COLOR]  = layer.getSolidColor();

    hw2d_rect_t crop = layer.getImageRect();
    cmd[G2DSFR_IMG_LEFT]   = crop.pos.hori;
    cmd[G2DSFR_IMG_TOP]    = crop.pos.vert;
    cmd[G2DSFR_IMG_RIGHT]  = crop.size.hori + crop.pos.hori;
    cmd[G2DSFR_IMG_BOTTOM] = crop.size.vert + crop.pos.vert;

    hw2d_rect_t window = layer.getTargetRect();
    if (area_is_zero(window))
        window.size = target_size;
    cmd[G2DSFR_SRC_DSTLEFT]   = window.pos.hori;
    cmd[G2DSFR_SRC_DSTTOP]    = window.pos.vert;
    cmd[G2DSFR_SRC_DSTRIGHT]  = window.size.hori + window.pos.hori;
    cmd[G2DSFR_SRC_DSTBOTTOM] = window.size.vert + window.pos.vert;

    uint8_t alpha = layer.getPlaneAlpha();
    cmd[G2DSFR_SRC_ALPHA] = (alpha << 24) | (alpha << 16) | (alpha << 8) | alpha;
    if ((layer.getCompositingMode() == HWC_BLENDING_PREMULT) ||
            (layer.getCompositingMode() == HWC2_BLEND_MODE_PREMULTIPLIED)) {
        cmd[G2DSFR_SRC_BLEND] = G2D_BLEND_SRCOVER;
    } else if ((layer.getCompositingMode() == HWC_BLENDING_COVERAGE) ||
               (layer.getCompositingMode() == HWC2_BLEND_MODE_COVERAGE)) {
        cmd[G2DSFR_SRC_BLEND] = G2D_BLEND_NONE;
    } else {
        cmd[G2DSFR_SRC_BLEND] = G2D_BLEND_SRCCOPY;
    }

    /* bottom layer always is opaque */
    if (index == 0) {
       cmd[G2DSFR_SRC_COMMAND] |= G2D_LAYERCMD_OPAQUE;
       if (alpha < 255)
           cmd[G2DSFR_SRC_COMMAND] |= G2D_LAYERCMD_PREMULT_GLOBALALPHA;
    } else {
       cmd[G2DSFR_SRC_COMMAND] |= G2D_LAYERCMD_ALPHABLEND;
    }

    return true;
}

bool AcrylicCompositorG2D9810::prepareSource(AcrylicLayer &layer, struct g2d_layer &image, uint32_t cmd[],
                                             hw2d_coord_t target_size, int index)
{
    if (layer.isSolidColor()) {
        prepareSolidLayer(layer, image, cmd, target_size, index);

        return true;
    }

    if (!prepareImage(layer, image, cmd, index))
        return false;

    cmd[G2DSFR_SRC_SELECT] = 0;

    hw2d_rect_t crop = layer.getImageRect();
    cmd[G2DSFR_IMG_LEFT]   = crop.pos.hori;
    cmd[G2DSFR_IMG_TOP]    = crop.pos.vert;
    cmd[G2DSFR_IMG_RIGHT]  = crop.size.hori + crop.pos.hori;
    cmd[G2DSFR_IMG_BOTTOM] = crop.size.vert + crop.pos.vert;

    hw2d_rect_t window = layer.getTargetRect();
    if (area_is_zero(window))
        window.size = target_size;
    cmd[G2DSFR_SRC_DSTLEFT]   = window.pos.hori;
    cmd[G2DSFR_SRC_DSTTOP]    = window.pos.vert;
    cmd[G2DSFR_SRC_DSTRIGHT]  = window.size.hori + window.pos.hori;
    cmd[G2DSFR_SRC_DSTBOTTOM] = window.size.vert + window.pos.vert;

    if (layer.isCompressed()) {
        cmd[G2DSFR_IMG_WIDTH]--;
        cmd[G2DSFR_IMG_HEIGHT]--;
    }

    cmd[G2DSFR_SRC_ROTATE] = 0;
    // HAL FLIP value: FLIP_H=0x01, FLIP_V=0x02
    // G2D FLIP value: FLIP_Y=0x05, FLIP_X=0x04
    unsigned int flip = layer.getTransform() & (HAL_TRANSFORM_FLIP_H | HAL_TRANSFORM_FLIP_V);
    if (!!(layer.getTransform() & HAL_TRANSFORM_ROT_90)) {
        window.size.swap();

        cmd[G2DSFR_SRC_ROTATE] |= G2D_ROTATEDIR_ROT90CCW;
        if (!flip || (flip == (HAL_TRANSFORM_FLIP_H | HAL_TRANSFORM_FLIP_V)))
            flip = ~flip & (HAL_TRANSFORM_FLIP_H | HAL_TRANSFORM_FLIP_V);
    }

    cmd[G2DSFR_SRC_ROTATE] |= flip << G2D_ROTATEDIR_FLIP_SHIFT;

    cmd[G2DSFR_SRC_XSCALE] = G2D_SCALE_FACTOR(crop.size.hori, window.size.hori);
    cmd[G2DSFR_SRC_YSCALE] = G2D_SCALE_FACTOR(crop.size.vert, window.size.vert);
    // Configure bilinear interpolation only if it is required.
    // Otherwise, G2D needs more bandwidth because it interpolates pixels
    // even though it is not required.
    if ((cmd[G2DSFR_SRC_XSCALE] | cmd[G2DSFR_SRC_YSCALE]) != G2D_SCALE_FACTOR(1, 1))
	    cmd[G2DSFR_SRC_SCALECONTROL] = G2D_SCALECONTROL_BILINEAR;
    else
	    cmd[G2DSFR_SRC_SCALECONTROL] = 0;

    // TODO: Configure initial phases according to the scale factors
     cmd[G2DSFR_SRC_XPHASE] = 0;
     cmd[G2DSFR_SRC_YPHASE] = 0;

    uint8_t alpha = layer.getPlaneAlpha();
    cmd[G2DSFR_SRC_ALPHA] = (alpha << 24) | (alpha << 16) | (alpha << 8) | alpha;
    if ((layer.getCompositingMode() == HWC_BLENDING_PREMULT) ||
            (layer.getCompositingMode() == HWC2_BLEND_MODE_PREMULTIPLIED)) {
        cmd[G2DSFR_SRC_BLEND] = G2D_BLEND_SRCOVER;
    } else if ((layer.getCompositingMode() == HWC_BLENDING_COVERAGE) ||
               (layer.getCompositingMode() == HWC2_BLEND_MODE_COVERAGE)) {
        cmd[G2DSFR_SRC_BLEND] = G2D_BLEND_NONE;
    } else {
        cmd[G2DSFR_SRC_BLEND] = G2D_BLEND_SRCCOPY;

        // HWC_BLEND_NONE is used not to appear its lower layer to target layer.
        // But, when G2D output is reused by DPU, lower layer could appear to target layer.
        // To prevent this, when blend mode is HWC_BLEND_NONE, make alpha channel max.
        // Example case is as follow.
        // If G2D composites several layers and topmost layer is HWC_BLEND_NONE
        // and has alpha lower than max, that alpha value remains in target buffer.
        // And if this result layer is recomposited with lower layer by DPU
        // lower layer color appears to final result layer.
        if ((cmd[G2DSFR_IMG_COLORMODE] == G2D_FMT_ABGR8888) ||
            (cmd[G2DSFR_IMG_COLORMODE] == G2D_FMT_ARGB8888) ||
            (cmd[G2DSFR_IMG_COLORMODE] == G2D_FMT_ABGR2101010)) {
            cmd[G2DSFR_IMG_COLORMODE] &= ~G2D_SWZ_ALPHA_MASK;
            cmd[G2DSFR_IMG_COLORMODE] |= G2D_SWZ_ALPHA_ONE;
        }
    }

    cmd[G2DSFR_SRC_COMMAND] = G2D_LAYERCMD_VALID;

    /* bottom layer always is opaque */
    if (index == 0) {
       cmd[G2DSFR_SRC_COMMAND] |= G2D_LAYERCMD_OPAQUE;
       if (alpha < 255)
           cmd[G2DSFR_SRC_COMMAND] |= G2D_LAYERCMD_PREMULT_GLOBALALPHA;
    } else {
       cmd[G2DSFR_SRC_COMMAND] |= G2D_LAYERCMD_ALPHABLEND;
    }

    cmd[G2DSFR_SRC_YCBCRMODE] = 0;
    cmd[G2DSFR_SRC_HDRMODE] = 0;

    return true;
}

bool AcrylicCompositorG2D9810::reallocLayer(unsigned int layercount)
{
    if (mMaxSourceCount >= layercount)
        return true;

    if (!mTask.commands.target) {
        mTask.commands.target = new uint32_t[G2DSFR_DST_FIELD_COUNT];
        if (!mTask.commands.target) {
            ALOGE("Failed to allocate command buffer for target image");
            return false;
        }

	memset(mTask.commands.target, 0, sizeof(uint32_t) * G2DSFR_DST_FIELD_COUNT);
    }

    delete [] mTask.source;
    for (unsigned int i = 0; i < mMaxSourceCount; i++)
        delete [] mTask.commands.source[i];

    mMaxSourceCount = 0;

    mTask.source = new g2d_layer[layercount];
    if (!mTask.source) {
        ALOGE("Failed to allocate %u source image descriptors", layercount);
        return false;
    }

    for (unsigned int i = 0; i < layercount; i++) {
        mTask.commands.source[i] = new uint32_t[G2DSFR_SRC_FIELD_COUNT];
        if (mTask.commands.source[i] == NULL) {
            ALOGE("Failed to allocate command buffer for source image");
            while (i-- > 0)
                delete [] mTask.commands.source[i];

            delete [] mTask.source;
            mTask.source = NULL;

            return false;
        }

	memset(mTask.commands.source[i], 0, sizeof(uint32_t) * G2DSFR_SRC_FIELD_COUNT);
    }

    mMaxSourceCount = layercount;

    return true;
}

int AcrylicCompositorG2D9810::ioctlG2D(void)
{
    if (mVersion == 1) {
        if (mDev.ioctl(G2D_IOC_PROCESS, &mTask) < 0)
            return -errno;
    } else {
        struct g2d_compat_task task;

        memcpy(&task, &mTask, sizeof(mTask) - sizeof(mTask.commands));
        memcpy(task.commands.target, mTask.commands.target, sizeof(task.commands.target));

        for (unsigned int i = 0; i < mMaxSourceCount; i++)
            task.commands.source[i] = mTask.commands.source[i];

        task.commands.extra = mTask.commands.extra;
        task.commands.num_extra_regs = mTask.commands.num_extra_regs;

        if (mDev.ioctl(G2D_IOC_COMPAT_PROCESS, &task) < 0)
            return -errno;

        mTask.flags = task.flags;
        mTask.laptime_in_usec = task.laptime_in_usec;

        for (unsigned int i = 0; i < mTask.num_release_fences; i++)
            mTask.release_fence[i] = task.release_fence[i];
    }

    return 0;
}

static const unsigned int EOTF_LUT_VALUES = 129;
static const unsigned int OETF_LUT_VALUES = 33;
static const unsigned int GM_LUT_VALUES = 9;
static const unsigned int TM_LUT_VALUES = 33;

static const unsigned int EOTF_COEF_X_COUNT = EOTF_LUT_VALUES / 2 + EOTF_LUT_VALUES % 2;
static const unsigned int EOTF_COEF_Y_COUNT = EOTF_LUT_VALUES;
static const unsigned int EOTF_COEF_COUNT = EOTF_COEF_X_COUNT + EOTF_COEF_Y_COUNT;

static const unsigned int OETF_COEF_X_COUNT = OETF_LUT_VALUES / 2 + OETF_LUT_VALUES % 2;
static const unsigned int OETF_COEF_Y_COUNT = OETF_LUT_VALUES / 2 + OETF_LUT_VALUES % 2;
static const unsigned int OETF_COEF_COUNT = OETF_COEF_X_COUNT + OETF_COEF_Y_COUNT;

static const unsigned int TM_COEF_X_COUNT = TM_LUT_VALUES / 2 + TM_LUT_VALUES % 2;
static const unsigned int TM_COEF_Y_COUNT = TM_LUT_VALUES;
// TM_COEF, TM_RNGX, TM_RNGY
static const unsigned int TM_COEF_COUNT = TM_COEF_X_COUNT + TM_COEF_Y_COUNT + 3;

static const unsigned int EOTF_X_BITS = 10;
static const unsigned int EOTF_Y_BITS = 16;
static const unsigned int OETF_X_BITS = 16;
static const unsigned int OETF_Y_BITS = 10;
static const unsigned int GM_BITS = 19;
static const unsigned int TM_X_BITS = 16;
static const unsigned int TM_Y_BITS = 20;

static const unsigned int MOD_CTRL_EEN = 1;
static const unsigned int MOD_CTRL_GEN = 2;
static const unsigned int MOD_CTRL_OEN = 0;
static const unsigned int MOD_CTRL_TEN = 5;

#define NUM_HDR_CTRL (1 + MAX_HDR_SET) // COM_CTRL, MOD_CTRL
#define NUM_HDR_COEF (MAX_HDR_SET * (OETF_COEF_COUNT + EOTF_COEF_COUNT + TM_COEF_COUNT + GM_LUT_VALUES))

#define NUM_HDR_REGS (NUM_HDR_COEF + NUM_HDR_CTRL)

enum {
    HDR_PROC_OETF,
    HDR_PROC_EOTF,
    HDR_PROC_GM,
    HDR_PROC_TM,
    NUM_HDR_PROC,
};

struct hdr_reg_info {
    unsigned int count;
    unsigned int bits;
    bool pair;
};

#define BITMASK(bits) ((1 << (bits)) - 1)
#define COEF(val, bits) ((val) & BITMASK(bits))
#define COEF_COUPLE_VALUE(val0, val1, bits) ((COEF(val1, bits) << 16) | COEF(val0, bits))

#define TM_COEF_BITS 10
#define TM_RNGX_BITS 16
#define TM_RNGY_BITS 9

#define TM_COEF_VALUE(val0, val1, val2) \
    ((COEF(val2, TM_COEF_BITS) << (2 * TM_COEF_BITS)) | \
     (COEF(val1, TM_COEF_BITS) << TM_COEF_BITS) | COEF(val0, TM_COEF_BITS))
#define TM_RNGX_VALUE(val0, val1) (COEF_COUPLE_VALUE(val0, val1, TM_RNGX_BITS))
#define TM_RNGY_VALUE(val0, val1) ((COEF(val1, TM_RNGY_BITS) << TM_RNGY_BITS) | COEF(val0, TM_RNGY_BITS))

#define G2D_HDR_TM_COEF 0x3434
#define G2D_HDR_TM_RNGX 0x3438
#define G2D_HDR_TM_RNGY 0x343C

#define G2D_HDR_COM_CTRL 0x3004
#define G2D_HDR_MOD_CTRL 0x3008
#define G2D_BASE_HDR_COEF(base, i) ((base) + ((i) * 0x800))

static const struct {
    int ctrl_bit;
    hdr_reg_info x;
    hdr_reg_info y;
} hdr_proc_info[NUM_HDR_PROC] = {
    { MOD_CTRL_OEN, { OETF_COEF_X_COUNT, OETF_X_BITS, 1 }, { OETF_COEF_Y_COUNT, OETF_Y_BITS, 1 }, }, // OETF
    { MOD_CTRL_EEN, { EOTF_COEF_X_COUNT, EOTF_X_BITS, 1 }, { EOTF_COEF_Y_COUNT, EOTF_Y_BITS, 0 }, }, // EOTF
    { MOD_CTRL_GEN, {     GM_LUT_VALUES,     GM_BITS, 0 }, {                 0,           0, 0 }, }, // GM
    { MOD_CTRL_TEN, {   TM_COEF_X_COUNT,   TM_X_BITS, 1 }, {   TM_COEF_Y_COUNT,   TM_Y_BITS, 0 }, }, // TM
};

// {X, Y}
static const uint32_t hdr_reg_base[NUM_HDR_PROC][2] = {
    { 0x300C, 0x3050 },
    { 0x3094, 0x3198 },
    { 0x339C, 0x0    },
    { 0x344C, 0x3490 },
};

static inline uint32_t getRegBaseX(unsigned int hdr, unsigned int proc)
{
    return hdr_reg_base[proc][0] + hdr * 0x800;
}

static inline uint32_t getRegBaseY(unsigned int hdr, unsigned int proc)
{
    return hdr_reg_base[proc][1] + hdr * 0x800;
}

void AcrylicCompositorG2D9810::setHdrLayerCommand(g2d_task &task, unsigned int layer_premult)
{
    for (unsigned int i = 0; i < task.num_source; i++) {
        for (unsigned int j = 0; j < MAX_HDR_SET; j++) {
            if (mHdrLibLayerMap[j] & (1 << i)) {
                task.commands.source[i][G2DSFR_SRC_HDRMODE] = j;
                if (layer_premult & (1 << i)) {
                    task.commands.source[i][G2DSFR_SRC_COMMAND] |= G2D_LAYERCMD_PREMULT_ALPHA;
                    task.commands.source[i][G2DSFR_SRC_HDRMODE] |= G2D_LAYER_HDRMODE_DEMULT_ALPHA;
                }
                break;
            }
        }
    }
}

static unsigned int write(g2d_reg regs[], unsigned int *data, int offset, const hdr_reg_info &info)
{
    if (!data)
        return 0;

    for (unsigned int i = 0; i < info.count; i++) {
        if (info.pair) {
            if (i == info.count - 1)
                regs[i].value = COEF(data[i * 2], info.bits);
            else
                regs[i].value = COEF_COUPLE_VALUE(data[i * 2], data[i * 2 + 1], info.bits);
        } else {
            regs[i].value = COEF(data[i], info.bits);
        }

        regs[i].offset = offset;
        offset += sizeof(uint32_t);
    }

    return info.count;
}

unsigned int AcrylicCompositorG2D9810::setHdrLibCommand(g2d_reg regs[])
{
    unsigned int mod_ctrl_bit, count = 0;
    unsigned int proc_en;

    for (unsigned int i = 0; i < MAX_HDR_SET; i++) {
        if (!mHdrLibCoef[i].hdr_en)
            continue;

        unsigned int *x[NUM_HDR_PROC] = { mHdrLibCoef[i].oetf_x, mHdrLibCoef[i].eotf_x,
            mHdrLibCoef[i].gm_coef, mHdrLibCoef[i].tm_x };
        unsigned int *y[NUM_HDR_PROC] = { mHdrLibCoef[i].oetf_y, mHdrLibCoef[i].eotf_y,
            nullptr, mHdrLibCoef[i].tm_y };

        mod_ctrl_bit = 0;
        proc_en = (!!(mHdrLibCoef[i].oetf_en) << HDR_PROC_OETF) |
            (!!(mHdrLibCoef[i].eotf_en) << HDR_PROC_EOTF) |
            (!!(mHdrLibCoef[i].gm_en) << HDR_PROC_GM) |
            (!!(mHdrLibCoef[i].tm_en) << HDR_PROC_TM);

        for (unsigned int j = 0; j < NUM_HDR_PROC; j++) {
            if (!(proc_en & (1 << j)))
                continue;

            mod_ctrl_bit |= (1 << hdr_proc_info[j].ctrl_bit);
            count += write(regs + count, x[j], getRegBaseX(i, j), hdr_proc_info[j].x);
            count += write(regs + count, y[j], getRegBaseY(i, j), hdr_proc_info[j].y);
        }
        // TM_RNGX, TM_RNGY, TM_COEF
        if (mHdrLibCoef[i].tm_en) {
            regs[count].value = TM_RNGX_VALUE(mHdrLibCoef[i].tm_rngx[0], mHdrLibCoef[i].tm_rngx[1]);
            regs[count].offset = G2D_BASE_HDR_COEF(G2D_HDR_TM_RNGX, i);
            count++;

            regs[count].value = TM_RNGY_VALUE(mHdrLibCoef[i].tm_rngy[0], mHdrLibCoef[i].tm_rngy[1]);
            regs[count].offset = G2D_BASE_HDR_COEF(G2D_HDR_TM_RNGY, i);
            count++;

            regs[count].value = TM_COEF_VALUE(mHdrLibCoef[i].tm_coef[0], mHdrLibCoef[i].tm_coef[1], mHdrLibCoef[i].tm_coef[2]);
            regs[count].offset = G2D_BASE_HDR_COEF(G2D_HDR_TM_COEF, i);
            count++;
        }
        regs[count].value = mod_ctrl_bit;
        regs[count].offset = G2D_BASE_HDR_COEF(G2D_HDR_MOD_CTRL, i);
        count++;
    }

    if (!count)
        return 0;

    regs[count].value = 1;
    regs[count].offset = G2D_HDR_COM_CTRL;
    count++;

    return count;
}

bool AcrylicCompositorG2D9810::executeG2D(int fence[], unsigned int num_fences, bool nonblocking)
{
    if (!validateAllLayers())
        return false;

    unsigned int layercount = layerCount();

    // Set invalid fence fd to the entries exceeds the number of source and destination images
    for (unsigned int i = layercount; i < num_fences; i++)
        fence[i] = -1;

    if (num_fences > layercount + 1)
        num_fences = layercount + 1;

    bool hasBackground = hasBackgroundColor();

    g2d_fmt *g2dfmt = halfmt_to_g2dfmt(halfmt_to_g2dfmt_tbl, len_halfmt_to_g2dfmt_tbl, getCanvas().getFormat());
    if (g2dfmt && (g2dfmt->g2dfmt & G2D_DATAFORMAT_SBWC))
        hasBackground = true;

    if (hasBackground) {
        layercount++;

        if (layercount > getCapabilities().maxLayerCount()) {
            ALOGE("Too many layers %d with the default background color configured", layerCount());
            return false;
        }
    }

    if (!reallocLayer(layercount))
        return false;

    sortLayers();

    mTask.flags = 0;

    if (!prepareImage(getCanvas(), mTask.target, mTask.commands.target, -1)) {
        ALOGE("Failed to configure the target image");
        return false;
    }

    if (getCanvas().isOTF())
        mTask.flags |= G2D_FLAG_HWFC;

    unsigned int baseidx = 0;

    if (hasBackground) {
        baseidx++;
        prepareSolidLayer(getCanvas(), mTask.source[0], mTask.commands.source[0]);
    }

    mTask.commands.target[G2DSFR_DST_YCBCRMODE] = 0;

    CSCMatrixWriter cscMatrixWriter(mTask.commands.target[G2DSFR_IMG_COLORMODE],
                                    getCanvas().getDataspace(),
                                    &mTask.commands.target[G2DSFR_DST_YCBCRMODE]);

    mTask.commands.target[G2DSFR_DST_YCBCRMODE] |= (G2D_LAYER_YCBCRMODE_OFFX | G2D_LAYER_YCBCRMODE_OFFY);

    unsigned int layer_premult = 0;
    for (unsigned int i = baseidx; i < layercount; i++) {
        AcrylicLayer &layer = *getLayer(i - baseidx);

        if (!prepareSource(layer, mTask.source[i],
                           mTask.commands.source[i], getCanvas().getImageDimension(),
                           i - baseidx)) {
            ALOGE("Failed to configure source layer %u", i - baseidx);
            return false;
        }

        if (!cscMatrixWriter.configure(mTask.commands.source[i][G2DSFR_IMG_COLORMODE],
                                       layer.getDataspace(),
                                       &mTask.commands.source[i][G2DSFR_SRC_YCBCRMODE])) {
            ALOGE("Failed to configure CSC coefficient of layer %d for dataspace %u",
                  i, layer.getDataspace());
            return false;
        }

        mHdrWriter.setLayerStaticMetadata(i, layer.getDataspace(),
                                          layer.getMinMasteringLuminance(),
                                          layer.getMaxMasteringLuminance());

        bool alpha_premult = (layer.getCompositingMode() == HWC_BLENDING_PREMULT)
                             || (layer.getCompositingMode() == HWC2_BLEND_MODE_PREMULTIPLIED);

        if (alpha_premult)
            layer_premult |= 1 << i;

        mHdrWriter.setLayerImageInfo(i, layer.getFormat(), alpha_premult);

        if (layer.getLayerData())
            mHdrWriter.setLayerOpaqueData(i, layer.getLayerData(), layer.getLayerDataLength());
    }

    mHdrWriter.setTargetInfo(getCanvas().getDataspace(), getTargetDisplayInfo());
    mHdrWriter.setTargetDisplayLuminance(getMinTargetDisplayLuminance(), getMaxTargetDisplayLuminance());

    mHdrWriter.getCommands();
    mHdrWriter.getLayerHdrMode(mTask);

    mTask.num_source = layercount;

    if (nonblocking)
        mTask.flags |= G2D_FLAG_NONBLOCK;

    mTask.num_release_fences = num_fences;
    mTask.release_fence = reinterpret_cast<int *>(alloca(sizeof(int) * num_fences));

    mTask.commands.num_extra_regs = cscMatrixWriter.getRegisterCount() + mHdrWriter.getCommandCount();

    // If mHdrWriter is disabled and command of hdr library exist, we use the library coefficients.
    // We use max hdr register count because we could not calculate the count here.
    // num_extra_regs is updated after to set the hdr register unlike mHdrWriter.
    unsigned int num_hdrlib_coef = 0;
    if (!mHdrWriter.getCommandCount()) {
        for (unsigned int i = 0; i < MAX_HDR_SET; i++) {
            if (mHdrLibCoef[i].hdr_en) {
                num_hdrlib_coef = NUM_HDR_REGS;
                break;
            }
        }
    }
    mTask.commands.extra = reinterpret_cast<g2d_reg *>(
            alloca(sizeof(g2d_reg) * (mTask.commands.num_extra_regs + num_hdrlib_coef)));

    unsigned int count = cscMatrixWriter.write(mTask.commands.extra);

    if (mHdrWriter.getCommandCount()) {
        mHdrWriter.write(mTask.commands.extra + count);
    } else if (num_hdrlib_coef) {
        mTask.commands.num_extra_regs += setHdrLibCommand(mTask.commands.extra + count);
        setHdrLayerCommand(mTask, layer_premult);
    }

    debug_show_g2d_task(mTask);

    if (ioctlG2D() < 0) {
        ALOGERR("Failed to process a task");
        show_g2d_task(mTask);
        return false;
    }

    mHdrWriter.putCommands();

    if (!!(mTask.flags & G2D_FLAG_ERROR)) {
        ALOGE("Error occurred during processing a task to G2D");
        show_g2d_task(mTask);
        return false;
    }

    getCanvas().clearSettingModified();
    getCanvas().setFence(-1);

    for (unsigned int i = 0; i < layerCount(); i++) {
        getLayer(i)->clearSettingModified();
        getLayer(i)->setFence(-1);
    }

    for (unsigned int i = 0; i < num_fences; i++)
        fence[i] = mTask.release_fence[i];

    return true;
}

bool AcrylicCompositorG2D9810::execute(int fence[], unsigned int num_fences)
{
    if (!executeG2D(fence, num_fences, true)) {
        // Clearing all acquire fences because their buffers are expired.
        // The clients should configure everything again to start new execution
        for (unsigned int i = 0; i < layerCount(); i++)
            getLayer(i)->setFence(-1);
        getCanvas().setFence(-1);

        return false;
    }

    return true;
}

bool AcrylicCompositorG2D9810::execute(int *handle)
{
    if (!executeG2D(NULL, 0, handle ? true : false)) {
        // Clearing all acquire fences because their buffers are expired.
        // The clients should configure everything again to start new execution
        for (unsigned int i = 0; i < layerCount(); i++)
            getLayer(i)->setFence(-1);
        getCanvas().setFence(-1);

        return false;
    }

    if (handle != NULL)
        *handle = 1; /* dummy handle */

    return true;
}

bool AcrylicCompositorG2D9810::waitExecution(int __unused handle)
{
    ALOGD_TEST("Waiting for execution of m2m1shot2 G2D completed by handle %d", handle);

    return true;
}

bool AcrylicCompositorG2D9810::requestPerformanceQoS(AcrylicPerformanceRequest *request)
{
    g2d_performance data;

    memset(&data, 0, sizeof(data));

    if (!request || (request->getFrameCount() == 0)) {
        if (mDev.ioctl(G2D_IOC_PERFORMANCE, &data) < 0) {
            ALOGERR("Failed to cancel performance request");
            return false;
        }

        ALOGD_TEST("Canceled performance request");
        return true;
    }

    ALOGD_TEST("Requesting performance: frame count %d:", request->getFrameCount());
    for (int i = 0; i < request->getFrameCount(); i++) {
        AcrylicPerformanceRequestFrame *frame = request->getFrame(i);
        uint64_t bandwidth = 0;
        bool src_yuv420;
        bool src_rotate;

        src_rotate = false;
        src_yuv420 = false;

        unsigned int bpp;
        for (int idx = 0; idx < frame->getLayerCount(); idx++) {
            AcrylicPerformanceRequestLayer *layer = &(frame->mLayers[idx]);
            uint64_t layer_bw, pixelcount;
            int32_t is_scaling;
            uint32_t src_hori = layer->mSourceRect.size.hori;
            uint32_t src_vert = layer->mSourceRect.size.vert;
            uint32_t dst_hori = layer->mTargetRect.size.hori;
            uint32_t dst_vert = layer->mTargetRect.size.vert;
            pixelcount = std::max(src_hori * src_vert, dst_hori * dst_vert);
            data.frame[i].layer[idx].crop_width = src_hori;
            data.frame[i].layer[idx].crop_height = src_vert;
            data.frame[i].layer[idx].window_width = dst_hori;
            data.frame[i].layer[idx].window_height = dst_vert;

            bpp = halfmt_bpp(layer->mPixFormat);
            if (bpp == 12) {
                data.frame[i].layer[idx].layer_attr |= G2D_PERF_LAYER_YUV2P;
                src_yuv420 = true;
            } else if (bpp == 15) {
                data.frame[i].layer[idx].layer_attr |= G2D_PERF_LAYER_YUV2P_82;
                src_yuv420 = true;
            }

            layer_bw = pixelcount * bpp;
            // Below is checking if scaling is involved.
            // Comparisons are replaced by additions to avoid branches.
            if (!!(layer->mTransform & HAL_TRANSFORM_ROT_90)) {
                src_rotate = true;
                data.frame[i].layer[idx].layer_attr |= G2D_PERF_LAYER_ROTATE;

                is_scaling = src_hori - dst_vert;
                is_scaling += src_vert - dst_hori;
            } else {
                is_scaling = src_hori - dst_hori;
                is_scaling += src_vert - dst_vert;
            }
            // Weight to the bandwidth when scaling is involved is 1.125.
            // It is multiplied by 16 to avoid multiplication with a real number.
            // We also get benefit from shift instead of multiplication.
            if (is_scaling == 0) {
                layer_bw <<= 4; // layer_bw * 16
            } else {
                layer_bw = (layer_bw << 4) + (layer_bw << 1); // layer_bw * 18

                data.frame[i].layer[idx].layer_attr |= G2D_PERF_LAYER_SCALING;
            }

            if (layer->mAttribute & AcrylicCanvas::ATTR_COMPRESSED)
                data.frame[i].layer[idx].layer_attr |= G2D_PERF_LAYER_COMPRESSED;

            bandwidth += layer_bw;
            ALOGD_TEST("        LAYER[%d]: BW %llu FMT %#x(%u) (%dx%d)@(%dx%d)on(%dx%d) --> (%dx%d)@(%dx%d) TRFM %#x",
                    idx, static_cast<unsigned long long>(layer_bw), layer->mPixFormat, bpp,
                    layer->mSourceRect.size.hori, layer->mSourceRect.size.vert,
                    layer->mSourceRect.pos.hori, layer->mSourceRect.pos.vert,
                    layer->mSourceDimension.hori, layer->mSourceDimension.vert,
                    layer->mTargetRect.size.hori, layer->mTargetRect.size.vert,
                    layer->mTargetRect.pos.hori, layer->mTargetRect.pos.vert, layer->mTransform);
        }

        bandwidth *= frame->mFrameRate;
        bandwidth >>= 17; // divide by 16(weight), 8(bpp) and 1024(kilobyte)

        data.frame[i].bandwidth_read = static_cast<uint32_t>(bandwidth);

        bpp = halfmt_bpp(frame->mTargetPixFormat);
        if (bpp == 12)
            data.frame[i].frame_attr |= G2D_PERF_FRAME_YUV2P;

        bandwidth = frame->mTargetDimension.hori * frame->mTargetDimension.vert;
        bandwidth *= static_cast<uint64_t>(frame->mFrameRate) * bpp;

        // RSH 12 : bw * 2 / (bits_per_byte * kilobyte)
        // RHS 13 : bw * 1 / (bits_per_byte * kilobyte)
        bandwidth >>= ((bpp == 12) && src_yuv420 && src_rotate) ? 12 : 13;
        data.frame[i].bandwidth_write = static_cast<uint32_t>(bandwidth);

        if (frame->mHasBackgroundLayer)
            data.frame[i].frame_attr |= G2D_PERF_FRAME_SOLIDCOLORFILL;

        data.frame[i].num_layers = frame->getLayerCount();
        data.frame[i].target_pixelcount = frame->mTargetDimension.vert * frame->mTargetDimension.hori;
        data.frame[i].frame_rate = frame->mFrameRate;

        ALOGD_TEST("    FRAME[%d]: BW:(%u, %u) Layercount %d, Framerate %d, Target %dx%d, FMT %#x Background? %d",
            i, data.frame[i].bandwidth_read, data.frame[i].bandwidth_write, data.frame[i].num_layers, frame->mFrameRate,
            frame->mTargetDimension.hori, frame->mTargetDimension.vert, frame->mTargetPixFormat,
            frame->mHasBackgroundLayer);
    }

    data.num_frame = request->getFrameCount();

    if (mDev.ioctl(G2D_IOC_PERFORMANCE, &data) < 0) {
        ALOGERR("Failed to request performance");
        return false;
    }

    return true;
}

int AcrylicCompositorG2D9810::prioritize(int priority)
{
    static int32_t g2d_priorities[] = {
            G2D_LOW_PRIORITY,     // 0
            G2D_MEDIUM_PRIORITY,  // 1
            G2D_HIGH_PRIORITY,    // 2
    };

    if (priority == mPriority)
        return 0;

    if (Acrylic::prioritize(priority) < 0)
        return -1;

    int32_t arg;

    if (priority > 2)
        arg = G2D_HIGHEST_PRIORITY;
    else if (priority < 0)
        arg = G2D_DEFAULT_PRIORITY;
    else
        arg = g2d_priorities[priority];

    if (mDev.ioctl(G2D_IOC_PRIORITY, &arg) < 0) {
        if (errno != EBUSY) {
            ALOGERR("Failed to set priority on a context of G2D");
            return -1;
        }

        ALOGD("G2D Driver returned EBUSY but the priority of %d(%d) is successfully applied", priority, arg);
        return 1;
    }

    ALOGD_TEST("Applied the priority of %d(%d) successfully", priority, arg);

    mPriority = priority;

    return 0;
}

void AcrylicCompositorG2D9810::setLibHdrCoefficient(int *layermap, void *hdrcoef)
{
    hdrCoef *coefs = static_cast<hdrCoef *>(hdrcoef);

    memcpy(mHdrLibLayerMap, layermap, sizeof(mHdrLibLayerMap));
    for (int i = 0; i < MAX_HDR_SET; i++)
        mHdrLibCoef[i] = coefs[i];
}

void AcrylicCompositorG2D9810::clearLibHdrCoefficient()
{
    memset(mHdrLibLayerMap, 0, sizeof(mHdrLibLayerMap));
    memset(mHdrLibCoef, 0, sizeof(mHdrLibCoef));
}
