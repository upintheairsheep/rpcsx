#include "device.hpp"
#include "tiler.hpp"

// #include "spirv-tools/libspirv.hpp"
#include "spirv-tools/optimizer.hpp"
#include "util/unreachable.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <forward_list>
#include <map>
#include <optional>
#include <set>
#include <shaders/rect_list.geom.h>
#include <span>
#include <util/VerifyVulkan.hpp>
// #include <spirv_glsl.hpp>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>

using namespace amdgpu;
using namespace amdgpu::device;

void *g_rwMemory;
std::size_t g_memorySize;
std::uint64_t g_memoryBase;

static const bool kUseDirectMemory = false;

static VkDevice g_vkDevice = VK_NULL_HANDLE;
static VkAllocationCallbacks *g_vkAllocator = nullptr;
static VkPhysicalDeviceMemoryProperties g_physicalMemoryProperties;
static VkPhysicalDeviceProperties g_physicalDeviceProperties;

MemoryZoneTable<StdSetInvalidationHandle> amdgpu::device::memoryZoneTable;

void device::setVkDevice(VkDevice device,
                         VkPhysicalDeviceMemoryProperties memProperties,
                         VkPhysicalDeviceProperties devProperties) {
  g_vkDevice = device;
  g_physicalMemoryProperties = memProperties;
  g_physicalDeviceProperties = devProperties;
}

static VkBlendFactor blendMultiplierToVkBlendFactor(BlendMultiplier mul) {
  switch (mul) {
  case kBlendMultiplierZero:
    return VK_BLEND_FACTOR_ZERO;
  case kBlendMultiplierOne:
    return VK_BLEND_FACTOR_ONE;
  case kBlendMultiplierSrcColor:
    return VK_BLEND_FACTOR_SRC_COLOR;
  case kBlendMultiplierOneMinusSrcColor:
    return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
  case kBlendMultiplierSrcAlpha:
    return VK_BLEND_FACTOR_SRC_ALPHA;
  case kBlendMultiplierOneMinusSrcAlpha:
    return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  case kBlendMultiplierDestAlpha:
    return VK_BLEND_FACTOR_DST_ALPHA;
  case kBlendMultiplierOneMinusDestAlpha:
    return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  case kBlendMultiplierDestColor:
    return VK_BLEND_FACTOR_DST_COLOR;
  case kBlendMultiplierOneMinusDestColor:
    return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
  case kBlendMultiplierSrcAlphaSaturate:
    return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
  case kBlendMultiplierConstantColor:
    return VK_BLEND_FACTOR_CONSTANT_COLOR;
  case kBlendMultiplierOneMinusConstantColor:
    return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
  case kBlendMultiplierSrc1Color:
    return VK_BLEND_FACTOR_SRC1_COLOR;
  case kBlendMultiplierInverseSrc1Color:
    return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
  case kBlendMultiplierSrc1Alpha:
    return VK_BLEND_FACTOR_SRC1_ALPHA;
  case kBlendMultiplierInverseSrc1Alpha:
    return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
  case kBlendMultiplierConstantAlpha:
    return VK_BLEND_FACTOR_CONSTANT_ALPHA;
  case kBlendMultiplierOneMinusConstantAlpha:
    return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
  }

  util::unreachable();
}

static VkBlendOp blendFuncToVkBlendOp(BlendFunc func) {
  switch (func) {
  case kBlendFuncAdd:
    return VK_BLEND_OP_ADD;
  case kBlendFuncSubtract:
    return VK_BLEND_OP_SUBTRACT;
  case kBlendFuncMin:
    return VK_BLEND_OP_MIN;
  case kBlendFuncMax:
    return VK_BLEND_OP_MAX;
  case kBlendFuncReverseSubtract:
    return VK_BLEND_OP_REVERSE_SUBTRACT;
  }

  util::unreachable();
}

static std::uint32_t
findPhysicalMemoryTypeIndex(std::uint32_t typeBits,
                            VkMemoryPropertyFlags properties) {
  typeBits &= (1 << g_physicalMemoryProperties.memoryTypeCount) - 1;

  while (typeBits != 0) {
    auto typeIndex = std::countr_zero(typeBits);

    if ((g_physicalMemoryProperties.memoryTypes[typeIndex].propertyFlags &
         properties) == properties) {
      return typeIndex;
    }

    typeBits &= ~(1 << typeIndex);
  }

  util::unreachable("Failed to find memory type with properties %x",
                    properties);
}

#define GNM_GET_FIELD(src, registername, field)                                \
  (((src) & (GNM_##registername##__##field##__MASK)) >>                        \
   (GNM_##registername##__##field##__SHIFT))

#define mmSQ_BUF_RSRC_WORD0 0x23C0
#define GNM_SQ_BUF_RSRC_WORD0__BASE_ADDRESS__MASK 0xffffffffL // size:32
#define GNM_SQ_BUF_RSRC_WORD0__BASE_ADDRESS__SHIFT 0

#define mmSQ_BUF_RSRC_WORD1 0x23C1
#define GNM_SQ_BUF_RSRC_WORD1__BASE_ADDRESS_HI__MASK 0x00000fffL // size:12
#define GNM_SQ_BUF_RSRC_WORD1__STRIDE__MASK 0x3fff0000L          // size:14
#define GNM_SQ_BUF_RSRC_WORD1__SWIZZLE_ENABLE__MASK 0x80000000L  // size: 1
#define GNM_SQ_BUF_RSRC_WORD1__BASE_ADDRESS_HI__SHIFT 0
#define GNM_SQ_BUF_RSRC_WORD1__STRIDE__SHIFT 16
#define GNM_SQ_BUF_RSRC_WORD1__SWIZZLE_ENABLE__SHIFT 31

#define mmSQ_BUF_RSRC_WORD2 0x23C2
#define GNM_SQ_BUF_RSRC_WORD2__NUM_RECORDS__MASK 0xffffffffL // size:32
#define GNM_SQ_BUF_RSRC_WORD2__NUM_RECORDS__SHIFT 0

#define mmSQ_BUF_RSRC_WORD3 0x23C3
#define GNM_SQ_BUF_RSRC_WORD3__DST_SEL_X__MASK 0x00000007L    // size: 3
#define GNM_SQ_BUF_RSRC_WORD3__DST_SEL_Y__MASK 0x00000038L    // size: 3
#define GNM_SQ_BUF_RSRC_WORD3__DST_SEL_Z__MASK 0x000001c0L    // size: 3
#define GNM_SQ_BUF_RSRC_WORD3__DST_SEL_W__MASK 0x00000e00L    // size: 3
#define GNM_SQ_BUF_RSRC_WORD3__ELEMENT_SIZE__MASK 0x00180000L // size: 2
#define GNM_SQ_BUF_RSRC_WORD3__INDEX_STRIDE__MASK 0x00600000L // size: 2
#define GNM_SQ_BUF_RSRC_WORD3__TYPE__MASK 0xc0000000L         // size: 2
#define GNM_SQ_BUF_RSRC_WORD3__DST_SEL_X__SHIFT 0
#define GNM_SQ_BUF_RSRC_WORD3__DST_SEL_Y__SHIFT 3
#define GNM_SQ_BUF_RSRC_WORD3__DST_SEL_Z__SHIFT 6
#define GNM_SQ_BUF_RSRC_WORD3__DST_SEL_W__SHIFT 9
#define GNM_SQ_BUF_RSRC_WORD3__ELEMENT_SIZE__SHIFT 19
#define GNM_SQ_BUF_RSRC_WORD3__INDEX_STRIDE__SHIFT 21
#define GNM_SQ_BUF_RSRC_WORD3__TYPE__SHIFT 30

#define mmCB_COLOR0_PITCH 0xA319
#define GNM_CB_COLOR0_PITCH__TILE_MAX__MASK 0x000007ffL       // size:11
#define GNM_CB_COLOR0_PITCH__FMASK_TILE_MAX__MASK 0x7ff00000L // size:11
#define GNM_CB_COLOR0_PITCH__TILE_MAX__SHIFT 0
#define GNM_CB_COLOR0_PITCH__FMASK_TILE_MAX__SHIFT 20

#define mmCB_COLOR0_SLICE 0xA31A
#define GNM_CB_COLOR0_SLICE__TILE_MAX__MASK 0x003fffffL // size:22
#define GNM_CB_COLOR0_SLICE__TILE_MAX__SHIFT 0

#define mmCB_COLOR0_VIEW 0xA31B
#define GNM_CB_COLOR0_VIEW__SLICE_START__MASK 0x000007ffL // size:11
#define GNM_CB_COLOR0_VIEW__SLICE_MAX__MASK 0x00ffe000L   // size:11
#define GNM_CB_COLOR0_VIEW__SLICE_START__SHIFT 0
#define GNM_CB_COLOR0_VIEW__SLICE_MAX__SHIFT 13

#define mmCB_COLOR0_INFO 0xA31C
#define GNM_CB_COLOR0_INFO__FAST_CLEAR__MASK 0x00002000L             // size: 1
#define GNM_CB_COLOR0_INFO__COMPRESSION__MASK 0x00004000L            // size: 1
#define GNM_CB_COLOR0_INFO__CMASK_IS_LINEAR__MASK 0x00080000L        // size: 1
#define GNM_CB_COLOR0_INFO__FMASK_COMPRESSION_MODE__MASK 0x0C000000L // size: 2
#define GNM_CB_COLOR0_INFO__DCC_ENABLE__MASK 0x10000000L             // size: 1
#define GNM_CB_COLOR0_INFO__CMASK_ADDR_TYPE__MASK 0x60000000L        // size: 2
#define GNM_CB_COLOR0_INFO__ALT_TILE_MODE__MASK 0x80000000L          // size: 1
#define GNM_CB_COLOR0_INFO__FAST_CLEAR__SHIFT 13
#define GNM_CB_COLOR0_INFO__COMPRESSION__SHIFT 14
#define GNM_CB_COLOR0_INFO__CMASK_IS_LINEAR__SHIFT 19
#define GNM_CB_COLOR0_INFO__FMASK_COMPRESSION_MODE__SHIFT 26
#define GNM_CB_COLOR0_INFO__DCC_ENABLE__SHIFT 28
#define GNM_CB_COLOR0_INFO__CMASK_ADDR_TYPE__SHIFT 29
#define GNM_CB_COLOR0_INFO__ALT_TILE_MODE__SHIFT 31
#define GNM_CB_COLOR0_INFO__FORMAT__MASK 0x3f << 2
#define GNM_CB_COLOR0_INFO__FORMAT__SHIFT 2

#define GNM_CB_COLOR0_INFO__ARRAY_MODE__MASK 0x0f << 8
#define GNM_CB_COLOR0_INFO__ARRAY_MODE__SHIFT 8

enum {
  ARRAY_LINEAR_GENERAL = 0x00, // Unaligned linear array
  ARRAY_LINEAR_ALIGNED = 0x01, // Aligned linear array
};

#define GNM_CB_COLOR0_INFO__NUMBER_TYPE__MASK 0x07 << 12
#define GNM_CB_COLOR0_INFO__NUMBER_TYPE__SHIFT 12

enum {
  NUMBER_UNORM = 0x00, // unsigned repeating fraction (urf): range [0..1], scale
                       // factor (2^n)-1
  NUMBER_SNORM = 0x01, // Microsoft-style signed rf: range [-1..1], scale factor
                       // (2^(n-1))-1
  NUMBER_USCALED = 0x02, // unsigned integer, converted to float in shader:
                         // range [0..(2^n)-1]
  NUMBER_SSCALED = 0x03, // signed integer, converted to float in shader: range
                         // [-2^(n-1)..2^(n-1)-1]
  NUMBER_UINT = 0x04, // zero-extended bit field, int in shader: not blendable
                      // or filterable
  NUMBER_SINT = 0x05, // sign-extended bit field, int in shader: not blendable
                      // or filterable
  NUMBER_SRGB = 0x06, // gamma corrected, range [0..1] (only suported for 8-bit
                      // components (always rounds color channels)
  NUMBER_FLOAT =
      0x07, // floating point, depends on component size: 32-bit: IEEE float,
            // SE8M23, bias 127, range (- 2^129..2^129) 24-bit: Depth float,
            // E4M20, bias 15, range [0..1] 16-bit: Short float SE5M10, bias 15,
            // range (-2^17..2^17) 11-bit: Packed float, E5M6 bias 15, range
            // [0..2^17) 10-bit: Packed float, E5M5 bias 15, range [0..2^17) all
            // other component sizes are treated as UINT
};

#define GNM_CB_COLOR0_INFO__READ_SIZE__MASK 1 << 15
#define GNM_CB_COLOR0_INFO__READ_SIZE__SHIFT 15

// Specifies how to map the red, green, blue, and alpha components from the
// shader to the components in the frame buffer pixel format. There are four
// choices for each number of components. With one component, the four modes
// select any one component. With 2-4 components, SWAP_STD selects the low order
// shader components in little-endian order; SWAP_ALT selects an alternate order
// (for 4 compoents) or inclusion of alpha (for 2 or 3 components); and the
// other two reverse the component orders for use on big-endian machines. The
// following table specifies the exact component mappings:
//
// 1 comp      std     alt     std_rev alt_rev
// ----------- ------- ------- ------- -------
// comp 0:     red     green   blue    alpha
//
// 3 comps     std     alt     std_rev alt_rev
// ----------- ------- ------- ------- -------
// comp 0:     red     red     green   alpha
// comp 1:     green   alpha   red     red
//
// 3 comps     std     alt     std_rev alt_rev
// ----------- ------- ------- ------- -------
// comp 0:     red     red     blue    alpha
// comp 1:     green   green   green   green
// comp 2:     blue    alpha   red     red
//
// 4 comps     std     alt     std_rev alt_rev
// ----------- ------- ------- ------- -------
// comp 0:     red     blue    alpha   alpha
// comp 1:     green   green   blue    red
// comp 2:     blue    red     green   green
// comp 3:     alpha   alpha   red     blue
//
#define GNM_CB_COLOR0_INFO__COMP_SWAP__MASK 0x03 << 16
#define GNM_CB_COLOR0_INFO__COMP_SWAP__SHIFT 16
enum {
  SWAP_STD = 0x00,     // standard little-endian comp order
  SWAP_ALT = 0x01,     // alternate components or order
  SWAP_STD_REV = 0x02, // reverses SWAP_STD order
  SWAP_ALT_REV = 0x03, // reverses SWAP_ALT order
};

// Specifies whether to clamp source data to the render target range prior to
// blending, in addition to the post- blend clamp. This bit must be zero for
// uscaled, sscaled and float number types and when blend_bypass is set.
#define GNM_CB_COLOR0_INFO__BLEND_CLAMP__MASK 1 << 20
#define GNM_CB_COLOR0_INFO__BLEND_CLAMP__SHIFT 20

// If false, use RGB=0.0 and A=1.0 (0x3f800000) to expand fast-cleared tiles. If
// true, use the CB_CLEAR register values to expand fast-cleared tiles.
#define GNM_CB_COLOR0_INFO__CLEAR_COLOR__MASK 1 << 21
#define GNM_CB_COLOR0_INFO__CLEAR_COLOR__SHIFT 21

// If false, blending occurs normaly as specified in CB_BLEND#_CONTROL. If true,
// blending (but not fog) is disabled. This must be set for the 24_8 and 8_24
// formats and when the number type is uint or sint. It should also be set for
// number types that are required to ignore the blend state in a specific
// aplication interface.
#define GNM_CB_COLOR0_INFO__BLEND_BYPASS__MASK 1 << 22
#define GNM_CB_COLOR0_INFO__BLEND_BYPASS__SHIFT 22

// If true, use 32-bit float precision for source colors, else truncate to
// 12-bit mantissa precision. This applies even if blending is disabled so that
// a null blend and blend disable produce the same result. This field is ignored
// for NUMBER_UINT and NUMBER_SINT. It must be one for floating point components
// larger than 16-bits or non- floating components larger than 12-bits,
// otherwise it must be 0.
#define GNM_CB_COLOR0_INFO__BLEND_FLOAT32__MASK 1 << 23
#define GNM_CB_COLOR0_INFO__BLEND_FLOAT32__SHIFT 23

// If false, floating point processing follows full IEEE rules for INF, NaN, and
// -0. If true, 0*anything produces 0 and no operation produces -0.
#define GNM_CB_COLOR0_INFO__SIMPLE_FLOAT__MASK 1 << 24
#define GNM_CB_COLOR0_INFO__SIMPLE_FLOAT__SHIFT 24

// This field selects between truncating (standard for floats) and rounding
// (standard for most other cases) to convert blender results to frame buffer
// components. The ROUND_BY_HALF setting can be over-riden by the DITHER_ENABLE
// field in CB_COLOR_CONTROL.
#define GNM_CB_COLOR0_INFO__ROUND_MODE__MASK 1 << 25
#define GNM_CB_COLOR0_INFO__ROUND_MODE__SHIFT 25

// This field indicates the allowed format for color data being exported from
// the pixel shader into the output merge block. This field may only be set to
// EXPORT_NORM if BLEND_CLAMP is enabled, BLEND_FLOAT32 is disabled, and the
// render target has only 11-bit or smaller UNORM or SNORM components. Selecting
// EXPORT_NORM flushes to zero values with exponent less than 0x70 (values less
// than 2^-15).
#define GNM_CB_COLOR0_INFO__SOURCE_FORMAT__MASK 1 << 27
#define GNM_CB_COLOR0_INFO__SOURCE_FORMAT__SHIFT 27

#define mmCB_COLOR0_ATTRIB 0xA31D
#define GNM_CB_COLOR0_ATTRIB__TILE_MODE_INDEX__MASK 0x0000001fL       // size: 5
#define GNM_CB_COLOR0_ATTRIB__FMASK_TILE_MODE_INDEX__MASK 0x000003e0L // size: 5
#define GNM_CB_COLOR0_ATTRIB__NUM_SAMPLES__MASK 0x00007000L           // size: 3
#define GNM_CB_COLOR0_ATTRIB__NUM_FRAGMENTS__MASK 0x00018000L         // size: 2
#define GNM_CB_COLOR0_ATTRIB__FORCE_DST_ALPHA_1__MASK 0x00020000L     // size: 1
#define GNM_CB_COLOR0_ATTRIB__TILE_MODE_INDEX__SHIFT 0
#define GNM_CB_COLOR0_ATTRIB__FMASK_TILE_MODE_INDEX__SHIFT 5
#define GNM_CB_COLOR0_ATTRIB__NUM_SAMPLES__SHIFT 12
#define GNM_CB_COLOR0_ATTRIB__NUM_FRAGMENTS__SHIFT 15
#define GNM_CB_COLOR0_ATTRIB__FORCE_DST_ALPHA_1__SHIFT 17

#define mmCB_COLOR0_DCC_CONTROL 0xA31E
#define GNM_CB_COLOR0_DCC_CONTROL__OVERWRITE_COMBINER_DISABLE__MASK            \
  0x00000001L // size: 1
#define GNM_CB_COLOR0_DCC_CONTROL__MAX_UNCOMPRESSED_BLOCK_SIZE__MASK           \
  0x0000000cL // size: 2
#define GNM_CB_COLOR0_DCC_CONTROL__MIN_COMPRESSED_BLOCK_SIZE__MASK             \
  0x00000010L // size: 1
#define GNM_CB_COLOR0_DCC_CONTROL__MAX_COMPRESSED_BLOCK_SIZE__MASK             \
  0x00000060L                                                        // size: 2
#define GNM_CB_COLOR0_DCC_CONTROL__COLOR_TRANSFORM__MASK 0x00000180L // size: 2
#define GNM_CB_COLOR0_DCC_CONTROL__INDEPENDENT_64B_BLOCKS__MASK                \
  0x00000200L // size: 1
#define GNM_CB_COLOR0_DCC_CONTROL__OVERWRITE_COMBINER_DISABLE__SHIFT 0
#define GNM_CB_COLOR0_DCC_CONTROL__MAX_UNCOMPRESSED_BLOCK_SIZE__SHIFT 2
#define GNM_CB_COLOR0_DCC_CONTROL__MIN_COMPRESSED_BLOCK_SIZE__SHIFT 4
#define GNM_CB_COLOR0_DCC_CONTROL__MAX_COMPRESSED_BLOCK_SIZE__SHIFT 5
#define GNM_CB_COLOR0_DCC_CONTROL__COLOR_TRANSFORM__SHIFT 7
#define GNM_CB_COLOR0_DCC_CONTROL__INDEPENDENT_64B_BLOCKS__SHIFT 9

#define mmCB_COLOR0_CMASK 0xA31F
#define GNM_CB_COLOR0_CMASK__BASE_256B__MASK 0xffffffffL // size:32
#define GNM_CB_COLOR0_CMASK__BASE_256B__SHIFT 0

#define mmCB_COLOR0_CMASK_SLICE 0xA320
#define GNM_CB_COLOR0_CMASK_SLICE__TILE_MAX__MASK 0x00003fffL // size:14
#define GNM_CB_COLOR0_CMASK_SLICE__TILE_MAX__SHIFT 0

#define mmCB_COLOR0_FMASK 0xA321
#define GNM_CB_COLOR0_FMASK__BASE_256B__MASK 0xffffffffL // size:32
#define GNM_CB_COLOR0_FMASK__BASE_256B__SHIFT 0

#define mmCB_COLOR0_FMASK_SLICE 0xA322
#define GNM_CB_COLOR0_FMASK_SLICE__TILE_MAX__MASK 0x003fffffL // size:22
#define GNM_CB_COLOR0_FMASK_SLICE__TILE_MAX__SHIFT 0

#define mmCB_COLOR0_CLEAR_WORD0 0xA323
#define GNM_CB_COLOR0_CLEAR_WORD0__CLEAR_WORD0__MASK 0xffffffffL // size:32
#define GNM_CB_COLOR0_CLEAR_WORD0__CLEAR_WORD0__SHIFT 0

#define mmCB_COLOR0_CLEAR_WORD1 0xA324
#define GNM_CB_COLOR0_CLEAR_WORD1__CLEAR_WORD1__MASK 0xffffffffL // size:32
#define GNM_CB_COLOR0_CLEAR_WORD1__CLEAR_WORD1__SHIFT 0

#define mmCB_COLOR0_DCC_BASE 0xA325
#define GNM_CB_COLOR0_DCC_BASE__BASE_256B__MASK 0xffffffffL // size:32
#define GNM_CB_COLOR0_DCC_BASE__BASE_256B__SHIFT 0

static constexpr auto CB_BLEND0_CONTROL_COLOR_SRCBLEND_MASK = genMask(0, 5);
static constexpr auto CB_BLEND0_CONTROL_COLOR_COMB_FCN_MASK =
    genMask(getMaskEnd(CB_BLEND0_CONTROL_COLOR_SRCBLEND_MASK), 3);
static constexpr auto CB_BLEND0_CONTROL_COLOR_DESTBLEND_MASK =
    genMask(getMaskEnd(CB_BLEND0_CONTROL_COLOR_COMB_FCN_MASK), 5);
static constexpr auto CB_BLEND0_CONTROL_OPACITY_WEIGHT_MASK =
    genMask(getMaskEnd(CB_BLEND0_CONTROL_COLOR_DESTBLEND_MASK), 1);
static constexpr auto CB_BLEND0_CONTROL_ALPHA_SRCBLEND_MASK =
    genMask(getMaskEnd(CB_BLEND0_CONTROL_OPACITY_WEIGHT_MASK) + 2, 5);
static constexpr auto CB_BLEND0_CONTROL_ALPHA_COMB_FCN_MASK =
    genMask(getMaskEnd(CB_BLEND0_CONTROL_ALPHA_SRCBLEND_MASK), 3);
static constexpr auto CB_BLEND0_CONTROL_ALPHA_DESTBLEND_MASK =
    genMask(getMaskEnd(CB_BLEND0_CONTROL_ALPHA_COMB_FCN_MASK), 5);
static constexpr auto CB_BLEND0_CONTROL_SEPARATE_ALPHA_BLEND_MASK =
    genMask(getMaskEnd(CB_BLEND0_CONTROL_ALPHA_DESTBLEND_MASK), 1);
static constexpr auto CB_BLEND0_CONTROL_BLEND_ENABLE_MASK =
    genMask(getMaskEnd(CB_BLEND0_CONTROL_SEPARATE_ALPHA_BLEND_MASK), 1);

static std::uint64_t pgmPsAddress = 0;
static std::uint64_t pgmVsAddress = 0;
static std::uint64_t pgmComputeAddress = 0;
static std::uint32_t userVsData[16];
static std::uint32_t userPsData[16];
static std::uint32_t userComputeData[16];
static std::uint32_t computeNumThreadX = 1;
static std::uint32_t computeNumThreadY = 1;
static std::uint32_t computeNumThreadZ = 1;
static std::uint8_t psUserSpgrs;
static std::uint8_t vsUserSpgrs;
static std::uint8_t computeUserSpgrs;

struct ColorBuffer {
  std::uint64_t base;
  std::uint8_t format;
  std::uint8_t tileModeIndex;

  void setRegister(unsigned index, std::uint32_t value) {
    switch (index) {
    case CB_COLOR0_BASE - CB_COLOR0_BASE:
      base = static_cast<std::uint64_t>(value) << 8;
      std::printf("  * base = %lx\n", base);
      break;

    case CB_COLOR0_PITCH - CB_COLOR0_BASE: {
      auto pitchTileMax = GNM_GET_FIELD(value, CB_COLOR0_PITCH, TILE_MAX);
      auto pitchFmaskTileMax =
          GNM_GET_FIELD(value, CB_COLOR0_PITCH, FMASK_TILE_MAX);
      std::printf("  * TILE_MAX = %lx\n", pitchTileMax);
      std::printf("  * FMASK_TILE_MAX = %lx\n", pitchFmaskTileMax);
      break;
    }
    case CB_COLOR0_SLICE - CB_COLOR0_BASE: { // SLICE
      auto sliceTileMax = GNM_GET_FIELD(value, CB_COLOR0_SLICE, TILE_MAX);
      std::printf("  * TILE_MAX = %lx\n", sliceTileMax);
      break;
    }
    case CB_COLOR0_VIEW - CB_COLOR0_BASE: { // VIEW
      auto viewSliceStart = GNM_GET_FIELD(value, CB_COLOR0_VIEW, SLICE_START);
      auto viewSliceMax = GNM_GET_FIELD(value, CB_COLOR0_VIEW, SLICE_MAX);

      std::printf("  * SLICE_START = %lx\n", viewSliceStart);
      std::printf("  * SLICE_MAX = %lx\n", viewSliceMax);
      break;
    }
    case CB_COLOR0_INFO - CB_COLOR0_BASE: { // INFO
      auto fastClear = GNM_GET_FIELD(value, CB_COLOR0_INFO, FAST_CLEAR);
      auto compression = GNM_GET_FIELD(value, CB_COLOR0_INFO, COMPRESSION);
      auto cmaskIsLinear =
          GNM_GET_FIELD(value, CB_COLOR0_INFO, CMASK_IS_LINEAR);
      auto fmaskCompressionMode =
          GNM_GET_FIELD(value, CB_COLOR0_INFO, FMASK_COMPRESSION_MODE);
      auto dccEnable = GNM_GET_FIELD(value, CB_COLOR0_INFO, DCC_ENABLE);
      auto cmaskAddrType =
          GNM_GET_FIELD(value, CB_COLOR0_INFO, CMASK_ADDR_TYPE);
      auto altTileMode = GNM_GET_FIELD(value, CB_COLOR0_INFO, ALT_TILE_MODE);
      format = GNM_GET_FIELD(value, CB_COLOR0_INFO, FORMAT);
      auto arrayMode = GNM_GET_FIELD(value, CB_COLOR0_INFO, ARRAY_MODE);
      auto numberType = GNM_GET_FIELD(value, CB_COLOR0_INFO, NUMBER_TYPE);
      auto readSize = GNM_GET_FIELD(value, CB_COLOR0_INFO, READ_SIZE);
      auto compSwap = GNM_GET_FIELD(value, CB_COLOR0_INFO, COMP_SWAP);
      auto blendClamp = GNM_GET_FIELD(value, CB_COLOR0_INFO, BLEND_CLAMP);
      auto clearColor = GNM_GET_FIELD(value, CB_COLOR0_INFO, CLEAR_COLOR);
      auto blendBypass = GNM_GET_FIELD(value, CB_COLOR0_INFO, BLEND_BYPASS);
      auto blendFloat32 = GNM_GET_FIELD(value, CB_COLOR0_INFO, BLEND_FLOAT32);
      auto simpleFloat = GNM_GET_FIELD(value, CB_COLOR0_INFO, SIMPLE_FLOAT);
      auto roundMode = GNM_GET_FIELD(value, CB_COLOR0_INFO, ROUND_MODE);
      auto sourceFormat = GNM_GET_FIELD(value, CB_COLOR0_INFO, SOURCE_FORMAT);

      std::printf("  * FAST_CLEAR = %lu\n", fastClear);
      std::printf("  * COMPRESSION = %lu\n", compression);
      std::printf("  * CMASK_IS_LINEAR = %lu\n", cmaskIsLinear);
      std::printf("  * FMASK_COMPRESSION_MODE = %lu\n", fmaskCompressionMode);
      std::printf("  * DCC_ENABLE = %lu\n", dccEnable);
      std::printf("  * CMASK_ADDR_TYPE = %lu\n", cmaskAddrType);
      std::printf("  * ALT_TILE_MODE = %lu\n", altTileMode);
      std::printf("  * FORMAT = %x\n", format);
      std::printf("  * ARRAY_MODE = %u\n", arrayMode);
      std::printf("  * NUMBER_TYPE = %u\n", numberType);
      std::printf("  * READ_SIZE = %u\n", readSize);
      std::printf("  * COMP_SWAP = %u\n", compSwap);
      std::printf("  * BLEND_CLAMP = %u\n", blendClamp);
      std::printf("  * CLEAR_COLOR = %u\n", clearColor);
      std::printf("  * BLEND_BYPASS = %u\n", blendBypass);
      std::printf("  * BLEND_FLOAT32 = %u\n", blendFloat32);
      std::printf("  * SIMPLE_FLOAT = %u\n", simpleFloat);
      std::printf("  * ROUND_MODE = %u\n", roundMode);
      std::printf("  * SOURCE_FORMAT = %u\n", sourceFormat);
      break;
    }

    case CB_COLOR0_ATTRIB - CB_COLOR0_BASE: { // ATTRIB
      tileModeIndex = GNM_GET_FIELD(value, CB_COLOR0_ATTRIB, TILE_MODE_INDEX);
      auto fmask_tile_mode_index =
          GNM_GET_FIELD(value, CB_COLOR0_ATTRIB, FMASK_TILE_MODE_INDEX);
      auto num_samples = GNM_GET_FIELD(value, CB_COLOR0_ATTRIB, NUM_SAMPLES);
      auto num_fragments =
          GNM_GET_FIELD(value, CB_COLOR0_ATTRIB, NUM_FRAGMENTS);
      auto force_dst_alpha_1 =
          GNM_GET_FIELD(value, CB_COLOR0_ATTRIB, FORCE_DST_ALPHA_1);

      std::printf("  * TILE_MODE_INDEX = %u\n", tileModeIndex);
      std::printf("  * FMASK_TILE_MODE_INDEX = %lu\n", fmask_tile_mode_index);
      std::printf("  * NUM_SAMPLES = %lu\n", num_samples);
      std::printf("  * NUM_FRAGMENTS = %lu\n", num_fragments);
      std::printf("  * FORCE_DST_ALPHA_1 = %lu\n", force_dst_alpha_1);
      break;
    }
    case CB_COLOR0_CMASK - CB_COLOR0_BASE: { // CMASK
      auto cmaskBase = GNM_GET_FIELD(value, CB_COLOR0_CMASK, BASE_256B) << 8;
      std::printf("  * cmaskBase = %lx\n", cmaskBase);
      break;
    }
    case CB_COLOR0_CMASK_SLICE - CB_COLOR0_BASE: { // CMASK_SLICE
      auto cmaskSliceTileMax =
          GNM_GET_FIELD(value, CB_COLOR0_CMASK_SLICE, TILE_MAX);
      std::printf("  * cmaskSliceTileMax = %lx\n", cmaskSliceTileMax);
      break;
    }
    case CB_COLOR0_FMASK - CB_COLOR0_BASE: { // FMASK
      auto fmaskBase = GNM_GET_FIELD(value, CB_COLOR0_FMASK, BASE_256B) << 8;
      std::printf("  * fmaskBase = %lx\n", fmaskBase);
      break;
    }
    case CB_COLOR0_FMASK_SLICE - CB_COLOR0_BASE: { // FMASK_SLICE
      auto fmaskSliceTileMax =
          GNM_GET_FIELD(value, CB_COLOR0_FMASK_SLICE, TILE_MAX);
      std::printf("  * fmaskSliceTileMax = %lx\n", fmaskSliceTileMax);
      break;
    }
    case CB_COLOR0_CLEAR_WORD0 - CB_COLOR0_BASE: // CLEAR_WORD0
      break;
    case CB_COLOR1_CLEAR_WORD0 - CB_COLOR0_BASE: // CLEAR_WORD1
      break;
    }
  }
};

static constexpr std::size_t colorBuffersCount = 6;

static ColorBuffer colorBuffers[colorBuffersCount];

static std::uint32_t indexType;

static std::uint32_t screenScissorX = 0;
static std::uint32_t screenScissorY = 0;
static std::uint32_t screenScissorW = 0;
static std::uint32_t screenScissorH = 0;

enum class CbColorFormat {
  /*
        00 - CB_DISABLE: Disables drawing to color
        buffer. Causes DB to not send tiles/quads to CB. CB
        itself ignores this field.
        01 - CB_NORMAL: Normal rendering mode. DB
        should send tiles and quads for pixel exports or just
        quads for compute exports.
        02 - CB_ELIMINATE_FAST_CLEAR: Fill fast
        cleared color surface locations with clear color. DB
        should send only tiles.
        03 - CB_RESOLVE: Read from MRT0, average all
        samples, and write to MRT1, which is one-sample. DB
        should send only tiles.
        04 - CB_DECOMPRESS: Decompress MRT0 to a
  */
  Disable,
  Normal,
  EliminateFastClear,
  Resolve,
};

enum class CbRasterOp {
  Blackness = 0x00,
  Nor = 0x05,          // ~(src | dst)
  AndInverted = 0x0a,  // ~src & dst
  CopyInverted = 0x0f, // ~src
  NotSrcErase = 0x11,  // ~src & ~dst
  SrcErase = 0x44,     // src & ~dst
  DstInvert = 0x55,    // ~dst
  Xor = 0x5a,          // src ^ dst
  Nand = 0x5f,         // ~(src & dst)
  And = 0x88,          // src & dst
  Equiv = 0x99,        // ~(src ^ dst)
  Noop = 0xaa,         // dst
  OrInverted = 0xaf,   // ~src | dst
  Copy = 0xcc,         // src
  OrReverse = 0xdd,    // src | ~dst
  Or = 0xEE,           // src | dst
  Whiteness = 0xff,
};

static CbColorFormat cbColorFormat = CbColorFormat::Normal;

static CbRasterOp cbRasterOp = CbRasterOp::Copy;

static std::uint32_t vgtPrimitiveType = 0;
static bool stencilEnable = false;
static bool depthEnable = false;
static bool depthWriteEnable = false;
static bool depthBoundsEnable = false;
static int zFunc = 0;
static bool backFaceEnable = false;
static int stencilFunc = 0;
static int stencilFuncBackFace = 0;

static float depthClear = 1.f;

static bool cullFront = false;
static bool cullBack = false;
static int face = 0; // 0 - CCW, 1 - CW
static bool polyMode = false;
static int polyModeFrontPType = 0;
static int polyModeBackPType = 0;
static bool polyOffsetFrontEnable = false;
static bool polyOffsetBackEnable = false;
static bool polyOffsetParaEnable = false;
static bool vtxWindowOffsetEnable = false;
static bool provokingVtxLast = false;
static bool erspCorrDis = false;
static bool multiPrimIbEna = false;

static bool depthClearEnable = false;
static bool stencilClearEnable = false;
static bool depthCopy = false;
static bool stencilCopy = false;
static bool resummarizeEnable = false;
static bool stencilCompressDisable = false;
static bool depthCompressDisable = false;
static bool copyCentroid = false;
static int copySample = 0;
static bool zpassIncrementDisable = false;

static std::uint64_t zReadBase = 0;
static std::uint64_t zWriteBase = 0;

static BlendMultiplier blendColorSrc = {};
static BlendFunc blendColorFn = {};
static BlendMultiplier blendColorDst = {};
static BlendMultiplier blendAlphaSrc = {};
static BlendFunc blendAlphaFn = {};
static BlendMultiplier blendAlphaDst = {};
static bool blendSeparateAlpha = false;
static bool blendEnable = false;
static std::uint32_t cbRenderTargetMask = 0;

static void setRegister(std::uint32_t regId, std::uint32_t value) {
  switch (regId) {
  case SPI_SHADER_PGM_LO_PS:
    pgmPsAddress &= ~((1ull << 40) - 1);
    pgmPsAddress |= static_cast<std::uint64_t>(value) << 8;
    break;
  case SPI_SHADER_PGM_HI_PS:
    pgmPsAddress &= (1ull << 40) - 1;
    pgmPsAddress |= static_cast<std::uint64_t>(value) << 40;
    break;
  case SPI_SHADER_PGM_LO_VS:
    pgmVsAddress &= ~((1ull << 40) - 1);
    pgmVsAddress |= static_cast<std::uint64_t>(value) << 8;
    break;
  case SPI_SHADER_PGM_HI_VS:
    pgmVsAddress &= (1ull << 40) - 1;
    pgmVsAddress |= static_cast<std::uint64_t>(value) << 40;
    break;

  case SPI_SHADER_USER_DATA_VS_0:
  case SPI_SHADER_USER_DATA_VS_1:
  case SPI_SHADER_USER_DATA_VS_2:
  case SPI_SHADER_USER_DATA_VS_3:
  case SPI_SHADER_USER_DATA_VS_4:
  case SPI_SHADER_USER_DATA_VS_5:
  case SPI_SHADER_USER_DATA_VS_6:
  case SPI_SHADER_USER_DATA_VS_7:
  case SPI_SHADER_USER_DATA_VS_8:
  case SPI_SHADER_USER_DATA_VS_9:
  case SPI_SHADER_USER_DATA_VS_10:
  case SPI_SHADER_USER_DATA_VS_11:
  case SPI_SHADER_USER_DATA_VS_12:
  case SPI_SHADER_USER_DATA_VS_13:
  case SPI_SHADER_USER_DATA_VS_14:
  case SPI_SHADER_USER_DATA_VS_15:
    userVsData[regId - SPI_SHADER_USER_DATA_VS_0] = value;
    break;

  case SPI_SHADER_USER_DATA_PS_0:
  case SPI_SHADER_USER_DATA_PS_1:
  case SPI_SHADER_USER_DATA_PS_2:
  case SPI_SHADER_USER_DATA_PS_3:
  case SPI_SHADER_USER_DATA_PS_4:
  case SPI_SHADER_USER_DATA_PS_5:
  case SPI_SHADER_USER_DATA_PS_6:
  case SPI_SHADER_USER_DATA_PS_7:
  case SPI_SHADER_USER_DATA_PS_8:
  case SPI_SHADER_USER_DATA_PS_9:
  case SPI_SHADER_USER_DATA_PS_10:
  case SPI_SHADER_USER_DATA_PS_11:
  case SPI_SHADER_USER_DATA_PS_12:
  case SPI_SHADER_USER_DATA_PS_13:
  case SPI_SHADER_USER_DATA_PS_14:
  case SPI_SHADER_USER_DATA_PS_15:
    userPsData[regId - SPI_SHADER_USER_DATA_PS_0] = value;
    break;

  case SPI_SHADER_PGM_RSRC2_PS:
    psUserSpgrs = (value >> 1) & 0x1f;
    break;

  case SPI_SHADER_PGM_RSRC2_VS:
    vsUserSpgrs = (value >> 1) & 0x1f;
    break;

  case CB_COLOR0_BASE ... CB_COLOR6_DCC_BASE: {
    auto buffer = (regId - CB_COLOR0_BASE) / (CB_COLOR1_BASE - CB_COLOR0_BASE);
    auto index = (regId - CB_COLOR0_BASE) % (CB_COLOR1_BASE - CB_COLOR0_BASE);
    colorBuffers[buffer].setRegister(index, value);
    break;
  }

  case DB_RENDER_CONTROL:
    depthClearEnable = getBit(value, 0);
    stencilClearEnable = getBit(value, 1);
    depthCopy = getBit(value, 2);
    stencilCopy = getBit(value, 3);
    resummarizeEnable = getBit(value, 4);
    stencilCompressDisable = getBit(value, 5);
    depthCompressDisable = getBit(value, 6);
    copyCentroid = getBit(value, 7);
    copySample = getBits(value, 10, 8);
    zpassIncrementDisable = getBit(value, 11);
    break;

  case DB_Z_READ_BASE:
    zReadBase = static_cast<std::uint64_t>(value) << 8;
    break;

  case DB_Z_WRITE_BASE:
    zWriteBase = static_cast<std::uint64_t>(value) << 8;
    break;

  case DB_DEPTH_CLEAR:
    depthClear = std::bit_cast<float>(value);
    break;

  case DB_DEPTH_CONTROL:
    stencilEnable = getBit(value, 0) != 0;
    depthEnable = getBit(value, 1) != 0;
    depthWriteEnable = getBit(value, 2) != 0;
    depthBoundsEnable = getBit(value, 3) != 0;
    zFunc = getBits(value, 6, 4);
    backFaceEnable = getBit(value, 7);
    stencilFunc = getBits(value, 11, 8);
    stencilFuncBackFace = getBits(value, 23, 20);

    std::printf("stencilEnable=%u, depthEnable=%u, depthWriteEnable=%u, "
                "depthBoundsEnable=%u, zFunc=%u, backFaceEnable=%u, "
                "stencilFunc=%u, stencilFuncBackFace=%u\n",
                stencilEnable, depthEnable, depthWriteEnable, depthBoundsEnable,
                zFunc, backFaceEnable, stencilFunc, stencilFuncBackFace);
    break;

  case CB_TARGET_MASK: {
    cbRenderTargetMask = value;
    break;
  }

  case CB_COLOR_CONTROL: {
    /*
      If true, then each UNORM format COLOR_8_8_8_8
      MRT is treated as an SRGB format instead. This affects
      both normal draw and resolve. This bit exists for
      compatibility with older architectures that did not have
      an SRGB number type.
    */
    auto degammaEnable = getBits(value, 3, 0);

    /*
      This field selects standard color processing or one of
      several major operation modes.

      POSSIBLE VALUES:
      00 - CB_DISABLE: Disables drawing to color
      buffer. Causes DB to not send tiles/quads to CB. CB
      itself ignores this field.
      01 - CB_NORMAL: Normal rendering mode. DB
      should send tiles and quads for pixel exports or just
      quads for compute exports.
      02 - CB_ELIMINATE_FAST_CLEAR: Fill fast
      cleared color surface locations with clear color. DB
      should send only tiles.
      03 - CB_RESOLVE: Read from MRT0, average all
      samples, and write to MRT1, which is one-sample. DB
      should send only tiles.
      04 - CB_DECOMPRESS: Decompress MRT0 to a
      uncompressed color format. This is required before a
      multisampled surface is accessed by the CPU, or used as
      a texture. This also decompresses the FMASK buffer. A
      CB_ELIMINATE_FAST_CLEAR pass before this is
      unnecessary. DB should send tiles and quads.
      05 - CB_FMASK_DECOMPRESS: Decompress the
      FMASK buffer into a texture readable format. A
      CB_ELIMINATE_FAST_CLEAR pass before this is
      unnecessary. DB should send only tiles.
    */
    auto mode = getBits(value, 6, 4);

    /*
      This field supports the 28 boolean ops that combine
      either source and dest or brush and dest, with brush
      provided by the shader in place of source. The code
      0xCC (11001100) copies the source to the destination,
      which disables the ROP function. ROP must be disabled
      if any MRT enables blending.

      POSSIBLE VALUES:
      00 - 0x00: BLACKNESS
      05 - 0x05
      10 - 0x0A
      15 - 0x0F
      17 - 0x11: NOTSRCERASE
      34 - 0x22
      51 - 0x33: NOTSRCCOPY
      68 - 0x44: SRCERASE
      80 - 0x50
      85 - 0x55: DSTINVERT
      90 - 0x5A: PATINVERT
      95 - 0x5F
      102 - 0x66: SRCINVERT
      119 - 0x77
      136 - 0x88: SRCAND
      153 - 0x99
      160 - 0xA0
      165 - 0xA5
      170 - 0xAA
      175 - 0xAF
      187 - 0xBB: MERGEPAINT
      204 - 0xCC: SRCCOPY
      221 - 0xDD
      238 - 0xEE: SRCPAINT
      240 - 0xF0: PATCOPY
      245 - 0xF5
      250 - 0xFA
      255 - 0xFF: WHITENESS
    */
    auto rop3 = getBits(value, 23, 16);

    std::printf("  * degammaEnable = %x\n", degammaEnable);
    std::printf("  * mode = %x\n", mode);
    std::printf("  * rop3 = %x\n", rop3);

    cbColorFormat = static_cast<CbColorFormat>(mode);
    cbRasterOp = static_cast<CbRasterOp>(rop3);
    break;
  }

  case PA_CL_CLIP_CNTL:
    cullFront = getBit(value, 0);
    cullBack = getBit(value, 1);
    face = getBit(value, 2);
    polyMode = getBits(value, 4, 3);
    polyModeFrontPType = getBits(value, 7, 5);
    polyModeBackPType = getBits(value, 10, 8);
    polyOffsetFrontEnable = getBit(value, 11);
    polyOffsetBackEnable = getBit(value, 12);
    polyOffsetParaEnable = getBit(value, 13);
    vtxWindowOffsetEnable = getBit(value, 16);
    provokingVtxLast = getBit(value, 19);
    erspCorrDis = getBit(value, 20);
    multiPrimIbEna = getBit(value, 21);
    break;

  case PA_SC_SCREEN_SCISSOR_TL:
    screenScissorX = static_cast<std::uint16_t>(value);
    screenScissorY = static_cast<std::uint16_t>(value >> 16);
    break;

  case PA_SC_SCREEN_SCISSOR_BR:
    screenScissorW = static_cast<std::uint16_t>(value) - screenScissorX;
    screenScissorH = static_cast<std::uint16_t>(value >> 16) - screenScissorY;
    break;

  case VGT_PRIMITIVE_TYPE:
    vgtPrimitiveType = value;
    break;

  case COMPUTE_NUM_THREAD_X:
    computeNumThreadX = value;
    break;

  case COMPUTE_NUM_THREAD_Y:
    computeNumThreadY = value;
    break;

  case COMPUTE_NUM_THREAD_Z:
    computeNumThreadZ = value;
    break;

  case COMPUTE_PGM_LO:
    pgmComputeAddress &= ~((1ull << 40) - 1);
    pgmComputeAddress |= static_cast<std::uint64_t>(value) << 8;
    break;

  case COMPUTE_PGM_HI:
    pgmComputeAddress &= (1ull << 40) - 1;
    pgmComputeAddress |= static_cast<std::uint64_t>(value) << 40;
    break;

  case COMPUTE_PGM_RSRC1:
    break;
  case COMPUTE_PGM_RSRC2:
    computeUserSpgrs = (value >> 1) & 0x1f;
    break;

  case COMPUTE_USER_DATA_0:
  case COMPUTE_USER_DATA_1:
  case COMPUTE_USER_DATA_2:
  case COMPUTE_USER_DATA_3:
  case COMPUTE_USER_DATA_4:
  case COMPUTE_USER_DATA_5:
  case COMPUTE_USER_DATA_6:
  case COMPUTE_USER_DATA_7:
  case COMPUTE_USER_DATA_8:
  case COMPUTE_USER_DATA_9:
  case COMPUTE_USER_DATA_10:
  case COMPUTE_USER_DATA_11:
  case COMPUTE_USER_DATA_12:
  case COMPUTE_USER_DATA_13:
  case COMPUTE_USER_DATA_14:
  case COMPUTE_USER_DATA_15:
    userComputeData[regId - COMPUTE_USER_DATA_0] = value;
    break;

  case CB_BLEND0_CONTROL: {
    blendColorSrc = (BlendMultiplier)fetchMaskedValue(
        value, CB_BLEND0_CONTROL_COLOR_SRCBLEND_MASK);
    blendColorFn = (BlendFunc)fetchMaskedValue(
        value, CB_BLEND0_CONTROL_COLOR_COMB_FCN_MASK);
    blendColorDst = (BlendMultiplier)fetchMaskedValue(
        value, CB_BLEND0_CONTROL_COLOR_DESTBLEND_MASK);
    auto opacity_weight =
        fetchMaskedValue(value, CB_BLEND0_CONTROL_OPACITY_WEIGHT_MASK);
    blendAlphaSrc = (BlendMultiplier)fetchMaskedValue(
        value, CB_BLEND0_CONTROL_ALPHA_SRCBLEND_MASK);
    blendAlphaFn = (BlendFunc)fetchMaskedValue(
        value, CB_BLEND0_CONTROL_ALPHA_COMB_FCN_MASK);
    blendAlphaDst = (BlendMultiplier)fetchMaskedValue(
        value, CB_BLEND0_CONTROL_ALPHA_DESTBLEND_MASK);
    blendSeparateAlpha =
        fetchMaskedValue(value, CB_BLEND0_CONTROL_SEPARATE_ALPHA_BLEND_MASK) !=
        0;
    blendEnable =
        fetchMaskedValue(value, CB_BLEND0_CONTROL_BLEND_ENABLE_MASK) != 0;

    std::printf("  * COLOR_SRCBLEND = %x\n", blendColorSrc);
    std::printf("  * COLOR_COMB_FCN = %x\n", blendColorFn);
    std::printf("  * COLOR_DESTBLEND = %x\n", blendColorDst);
    std::printf("  * OPACITY_WEIGHT = %x\n", opacity_weight);
    std::printf("  * ALPHA_SRCBLEND = %x\n", blendAlphaSrc);
    std::printf("  * ALPHA_COMB_FCN = %x\n", blendAlphaFn);
    std::printf("  * ALPHA_DESTBLEND = %x\n", blendAlphaDst);
    std::printf("  * SEPARATE_ALPHA_BLEND = %x\n", blendSeparateAlpha);
    std::printf("  * BLEND_ENABLE = %x\n", blendEnable);
    break;
  }
  }
}

void ShaderModule::destroy() const {
  if (descriptorPool) {
    vkDestroyDescriptorPool(g_vkDevice, descriptorPool, nullptr);
  }

  vkDestroyPipeline(g_vkDevice, pipeline, nullptr);
  vkDestroyPipelineLayout(g_vkDevice, pipelineLayout, nullptr);
  vkDestroyDescriptorSetLayout(g_vkDevice, descriptorSetLayout, nullptr);
}

DrawContext::~DrawContext() {
  for (auto shader : loadedShaderModules) {
    vkDestroyShaderModule(g_vkDevice, shader, nullptr);
  }
}

static void submitCommands(VkCommandPool commandPool, VkQueue queue, auto cb) {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandPool = commandPool;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer;
  vkAllocateCommandBuffers(g_vkDevice, &allocInfo, &commandBuffer);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  vkBeginCommandBuffer(commandBuffer, &beginInfo);
  cb(commandBuffer);

  vkEndCommandBuffer(commandBuffer);

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;

  Verify() << vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
  Verify() << vkQueueWaitIdle(queue);

  vkFreeCommandBuffers(g_vkDevice, commandPool, 1, &commandBuffer);
}

static VkShaderModule
createShaderModule(std::span<const std::uint32_t> shaderCode) {
  VkShaderModuleCreateInfo moduleCreateInfo{};
  moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  moduleCreateInfo.codeSize = shaderCode.size() * sizeof(std::uint32_t);
  moduleCreateInfo.pCode = shaderCode.data();

  VkShaderModule shaderModule;

  Verify() << vkCreateShaderModule(g_vkDevice, &moduleCreateInfo, nullptr,
                                   &shaderModule);
  return shaderModule;
}

static VkPipelineShaderStageCreateInfo
createPipelineShaderStage(DrawContext &dc,
                          std::span<const std::uint32_t> shaderCode,
                          VkShaderStageFlagBits stage) {
  VkPipelineShaderStageCreateInfo shaderStage = {};
  shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shaderStage.stage = stage;
  shaderStage.module = createShaderModule(shaderCode);
  shaderStage.pName = "main";
  Verify() << (shaderStage.module != VK_NULL_HANDLE);
  dc.loadedShaderModules.push_back(shaderStage.module);
  return shaderStage;
}

static void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image,
                                  VkImageAspectFlags aspectFlags,
                                  VkImageLayout oldLayout,
                                  VkImageLayout newLayout) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = aspectFlags;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  auto layoutToStageAccess = [](VkImageLayout layout)
      -> std::pair<VkPipelineStageFlags, VkAccessFlags> {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
      return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};

    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};

    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};

    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT};

    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
      return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT};

    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT};

    default:
      util::unreachable("unsupported layout transition! %d", layout);
    }
  };

  auto [sourceStage, sourceAccess] = layoutToStageAccess(oldLayout);
  auto [destinationStage, destinationAccess] = layoutToStageAccess(newLayout);

  barrier.srcAccessMask = sourceAccess;
  barrier.dstAccessMask = destinationAccess;

  vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0,
                       nullptr, 0, nullptr, 1, &barrier);
}

static void copyImageToBuffer(VkCommandBuffer commandBuffer, VkImage image,
                              VkBuffer buffer, uint32_t width, uint32_t height,
                              uint32_t bufferOffset, uint32_t bufferRowLength,
                              uint32_t bufferHeight,
                              VkImageAspectFlags aspectFlags) {
  VkBufferImageCopy region{};
  region.bufferOffset = bufferOffset;
  region.bufferRowLength = bufferRowLength;
  region.bufferImageHeight = bufferHeight;
  region.imageSubresource.aspectMask = aspectFlags;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {width, height, 1};

  vkCmdCopyImageToBuffer(commandBuffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                         &region);
}

static void copyBufferToImage(VkCommandBuffer cmdBuffer, VkImage image,
                              VkBuffer buffer, uint32_t width, uint32_t height,
                              uint32_t bufferOffset, uint32_t bufferRowLength,
                              uint32_t bufferHeight,
                              VkImageAspectFlags aspectFlags) {
  VkBufferImageCopy region{};
  region.bufferOffset = bufferOffset;
  region.bufferRowLength = bufferRowLength;
  region.bufferImageHeight = bufferHeight;
  region.imageSubresource.aspectMask = aspectFlags;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {width, height, 1};

  vkCmdCopyBufferToImage(cmdBuffer, buffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

static VkRenderPass createRenderPass(VkFormat colorFormat,
                                     VkFormat depthFormat) {
  std::array<VkAttachmentDescription, 2> attachments = {};
  // Color attachment
  attachments[0].format = colorFormat;
  attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
  attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  // Depth attachment
  attachments[1].format = depthFormat;
  attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
  attachments[1].loadOp = depthClearEnable ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                           : VK_ATTACHMENT_LOAD_OP_LOAD;
  attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[1].stencilLoadOp = stencilClearEnable
                                     ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                     : VK_ATTACHMENT_LOAD_OP_LOAD;
  attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[1].initialLayout =
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorReference = {};
  colorReference.attachment = 0;
  colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depthReference = {};
  depthReference.attachment = 1;
  depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpassDescription = {};
  subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpassDescription.colorAttachmentCount = 1;
  subpassDescription.pColorAttachments = &colorReference;
  subpassDescription.pDepthStencilAttachment = &depthReference;
  subpassDescription.inputAttachmentCount = 0;
  subpassDescription.pInputAttachments = nullptr;
  subpassDescription.preserveAttachmentCount = 0;
  subpassDescription.pPreserveAttachments = nullptr;
  subpassDescription.pResolveAttachments = nullptr;

  // Subpass dependencies for layout transitions
  std::array<VkSubpassDependency, 2> dependencies;

  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  VkRenderPassCreateInfo renderPassInfo = {};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
  renderPassInfo.pAttachments = attachments.data();
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpassDescription;
  renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
  renderPassInfo.pDependencies = dependencies.data();

  VkRenderPass renderPass;
  Verify() << vkCreateRenderPass(g_vkDevice, &renderPassInfo, nullptr,
                                 &renderPass);
  return renderPass;
}

static VkFramebuffer
createFramebuffer(VkRenderPass renderPass, VkExtent2D extent,
                  std::span<const VkImageView> attachments) {
  VkFramebufferCreateInfo frameBufferCreateInfo = {};
  frameBufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  frameBufferCreateInfo.pNext = NULL;
  frameBufferCreateInfo.renderPass = renderPass;
  frameBufferCreateInfo.attachmentCount = attachments.size();
  frameBufferCreateInfo.pAttachments = attachments.data();
  frameBufferCreateInfo.width = extent.width;
  frameBufferCreateInfo.height = extent.height;
  frameBufferCreateInfo.layers = 1;

  VkFramebuffer framebuffer;
  Verify() << vkCreateFramebuffer(g_vkDevice, &frameBufferCreateInfo, nullptr,
                                  &framebuffer);
  return framebuffer;
}

static VkPipeline createGraphicsPipeline(
    VkPipelineLayout pipelineLayout, VkRenderPass renderPass,
    VkPipelineCache pipelineCache,
    VkPipelineVertexInputStateCreateInfo vertexInputInfo,
    VkPrimitiveTopology topology,
    std::span<const VkPipelineShaderStageCreateInfo> shaders) {
  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = topology;
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_TRUE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.cullMode =
      (false && cullBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE) |
      (false && cullFront ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);

  rasterizer.frontFace =
      face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterizer.depthBiasEnable = VK_FALSE;
  // rasterizer.depthBiasConstantFactor = 0;
  // rasterizer.depthBiasClamp = 0;
  // rasterizer.depthBiasSlopeFactor = 0;
  rasterizer.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = depthEnable;
  depthStencil.depthWriteEnable = depthWriteEnable;
  depthStencil.depthCompareOp = (VkCompareOp)zFunc;
  depthStencil.depthBoundsTestEnable = depthBoundsEnable;
  // depthStencil.stencilTestEnable = stencilEnable;
  // depthStencil.front;
  // depthStencil.back;
  depthStencil.minDepthBounds = 0.f;
  depthStencil.maxDepthBounds = 1.f;

  VkPipelineColorBlendAttachmentState colorBlendAttachment{};

  colorBlendAttachment.blendEnable = blendEnable;
  colorBlendAttachment.srcColorBlendFactor =
      blendMultiplierToVkBlendFactor(blendColorSrc);
  colorBlendAttachment.dstColorBlendFactor =
      blendMultiplierToVkBlendFactor(blendColorDst);
  colorBlendAttachment.colorBlendOp = blendFuncToVkBlendOp(blendColorFn);

  if (blendSeparateAlpha) {
    colorBlendAttachment.srcAlphaBlendFactor =
        blendMultiplierToVkBlendFactor(blendAlphaSrc);
    colorBlendAttachment.dstAlphaBlendFactor =
        blendMultiplierToVkBlendFactor(blendAlphaDst);
    colorBlendAttachment.alphaBlendOp = blendFuncToVkBlendOp(blendAlphaFn);
  } else {
    colorBlendAttachment.srcAlphaBlendFactor =
        colorBlendAttachment.srcColorBlendFactor;
    colorBlendAttachment.dstAlphaBlendFactor =
        colorBlendAttachment.dstColorBlendFactor;
    colorBlendAttachment.alphaBlendOp = colorBlendAttachment.colorBlendOp;
  }

  colorBlendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.logicOp = VK_LOGIC_OP_COPY;
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorBlendAttachment;
  colorBlending.blendConstants[0] = 0.0f;
  colorBlending.blendConstants[1] = 0.0f;
  colorBlending.blendConstants[2] = 0.0f;
  colorBlending.blendConstants[3] = 0.0f;

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = shaders.size();
  pipelineInfo.pStages = shaders.data();
  pipelineInfo.pVertexInputState = &vertexInputInfo;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.layout = pipelineLayout;
  pipelineInfo.renderPass = renderPass;
  pipelineInfo.subpass = 0;
  pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

  std::array dynamicStateEnables = {VK_DYNAMIC_STATE_VIEWPORT,
                                    VK_DYNAMIC_STATE_SCISSOR};

  VkPipelineDynamicStateCreateInfo pipelineDynamicStateCreateInfo{};
  pipelineDynamicStateCreateInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  pipelineDynamicStateCreateInfo.pDynamicStates = dynamicStateEnables.data();
  pipelineDynamicStateCreateInfo.dynamicStateCount =
      static_cast<uint32_t>(dynamicStateEnables.size());

  pipelineInfo.pDynamicState = &pipelineDynamicStateCreateInfo;

  VkPipeline result;
  Verify() << vkCreateGraphicsPipelines(g_vkDevice, pipelineCache, 1,
                                        &pipelineInfo, nullptr, &result);

  return result;
}

static VkPipeline
createComputePipeline(VkPipelineLayout pipelineLayout,
                      VkPipelineCache pipelineCache,
                      const VkPipelineShaderStageCreateInfo &shader) {
  VkComputePipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipelineInfo.layout = pipelineLayout;
  pipelineInfo.stage = shader;

  VkPipeline result;
  Verify() << vkCreateComputePipelines(g_vkDevice, pipelineCache, 1,
                                       &pipelineInfo, nullptr, &result);
  return result;
}

static VkDescriptorSet createDescriptorSet(const ShaderModule *shader) {
  VkDescriptorSetAllocateInfo allocateInfo{};
  allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocateInfo.descriptorPool = shader->descriptorPool;
  allocateInfo.pSetLayouts = &shader->descriptorSetLayout;
  allocateInfo.descriptorSetCount = 1;

  VkDescriptorSet result;
  Verify() << vkAllocateDescriptorSets(g_vkDevice, &allocateInfo, &result);
  return result;
}

static VkDescriptorSetLayoutBinding
createDescriptorSetLayoutBinding(uint32_t binding, uint32_t descriptorCount,
                                 VkDescriptorType descriptorType,
                                 VkShaderStageFlags stageFlags) {

  VkDescriptorSetLayoutBinding result{};
  result.binding = binding;
  result.descriptorCount = descriptorCount;
  result.descriptorType = descriptorType;
  result.pImmutableSamplers = nullptr;
  result.stageFlags = stageFlags;
  return result;
}

static VkImageSubresourceRange
imageSubresourceRange(VkImageAspectFlags aspectMask, uint32_t baseMipLevel = 0,
                      uint32_t levelCount = 1, uint32_t baseArrayLayer = 0,
                      uint32_t layerCount = 1) {
  return {aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount};
}

static VkImageView createImageView2D(VkImage image, VkFormat format,
                                     VkComponentMapping components,
                                     VkImageSubresourceRange subresourceRange) {
  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = format;
  viewInfo.components = components;
  viewInfo.subresourceRange = subresourceRange;

  VkImageView imageView;
  Verify() << vkCreateImageView(g_vkDevice, &viewInfo, nullptr, &imageView);

  return imageView;
}

static void
updateDescriptorSets(std::span<const VkWriteDescriptorSet> writeSets,
                     std::span<const VkCopyDescriptorSet> copySets = {}) {
  vkUpdateDescriptorSets(g_vkDevice, writeSets.size(), writeSets.data(),
                         copySets.size(), copySets.data());
}

static VkDescriptorSetLayout createDescriptorSetLayout(
    std::span<const VkDescriptorSetLayoutBinding> bindings) {
  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
  layoutInfo.pBindings = bindings.data();

  VkDescriptorSetLayout result;
  Verify() << vkCreateDescriptorSetLayout(g_vkDevice, &layoutInfo, nullptr,
                                          &result);

  return result;
}

static VkDescriptorPoolSize createDescriptorPoolSize(VkDescriptorType type,
                                                     uint32_t descriptorCount) {
  VkDescriptorPoolSize result{};
  result.type = type;
  result.descriptorCount = descriptorCount;
  return result;
}

static VkDescriptorPool
createDescriptorPool(uint32_t maxSets,
                     std::span<const VkDescriptorPoolSize> poolSizes) {
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();
  poolInfo.maxSets = maxSets;

  VkDescriptorPool result;
  Verify() << vkCreateDescriptorPool(g_vkDevice, &poolInfo, nullptr, &result);
  return result;
}

static VkDescriptorBufferInfo
descriptorBufferInfo(VkBuffer buffer, VkDeviceSize offset = 0,
                     VkDeviceSize range = VK_WHOLE_SIZE) {
  return {buffer, offset, range};
}

static VkDescriptorImageInfo descriptorImageInfo(VkSampler sampler,
                                                 VkImageView imageView,
                                                 VkImageLayout imageLayout) {
  return {sampler, imageView, imageLayout};
}

static VkWriteDescriptorSet writeDescriptorSetBuffer(
    VkDescriptorSet dstSet, VkDescriptorType type, uint32_t binding,
    const VkDescriptorBufferInfo *bufferInfo, std::uint32_t count = 1) {
  VkWriteDescriptorSet writeDescriptorSet{};
  writeDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writeDescriptorSet.dstSet = dstSet;
  writeDescriptorSet.descriptorType = type;
  writeDescriptorSet.dstBinding = binding;
  writeDescriptorSet.pBufferInfo = bufferInfo;
  writeDescriptorSet.descriptorCount = count;
  return writeDescriptorSet;
}

static VkWriteDescriptorSet writeDescriptorSetImage(
    VkDescriptorSet dstSet, VkDescriptorType type, uint32_t binding,
    const VkDescriptorImageInfo *imageInfo, std::uint32_t count) {
  VkWriteDescriptorSet writeDescriptorSet{};
  writeDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writeDescriptorSet.dstSet = dstSet;
  writeDescriptorSet.descriptorType = type;
  writeDescriptorSet.dstBinding = binding;
  writeDescriptorSet.pImageInfo = imageInfo;
  writeDescriptorSet.descriptorCount = count;
  return writeDescriptorSet;
}

static VkPipelineLayout
createPipelineLayout(VkDescriptorSetLayout descriptorSetLayout) {
  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;

  VkPipelineLayout result;
  Verify() << vkCreatePipelineLayout(g_vkDevice, &pipelineLayoutInfo, nullptr,
                                     &result);
  return result;
}

static VkPipelineVertexInputStateCreateInfo createPipelineVertexInputState(
    std::span<const VkVertexInputBindingDescription> vertexBindingDescriptions,
    std::span<const VkVertexInputAttributeDescription>
        vertexAttributeDescriptions,
    VkPipelineVertexInputStateCreateFlags flags = 0) {
  VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
  vertexInputInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.flags = flags;

  vertexInputInfo.vertexBindingDescriptionCount =
      vertexBindingDescriptions.size();
  vertexInputInfo.pVertexBindingDescriptions = vertexBindingDescriptions.data();

  vertexInputInfo.vertexAttributeDescriptionCount =
      vertexAttributeDescriptions.size();
  vertexInputInfo.pVertexAttributeDescriptions =
      vertexAttributeDescriptions.data();

  return vertexInputInfo;
}

static int getBitWidthOfSurfaceFormat(SurfaceFormat format) {
  switch (format) {
  case kSurfaceFormatInvalid:
    return 0;
  case kSurfaceFormat8:
    return 8;
  case kSurfaceFormat16:
    return 16;
  case kSurfaceFormat8_8:
    return 8 + 8;
  case kSurfaceFormat32:
    return 32;
  case kSurfaceFormat16_16:
    return 16 + 16;
  case kSurfaceFormat10_11_11:
    return 10 + 11 + 11;
  case kSurfaceFormat11_11_10:
    return 11 + 11 + 10;
  case kSurfaceFormat10_10_10_2:
    return 10 + 10 + 10 + 2;
  case kSurfaceFormat2_10_10_10:
    return 2 + 10 + 10 + 10;
  case kSurfaceFormat8_8_8_8:
    return 8 + 8 + 8 + 8;
  case kSurfaceFormat32_32:
    return 32 + 32;
  case kSurfaceFormat16_16_16_16:
    return 16 + 16 + 16 + 16;
  case kSurfaceFormat32_32_32:
    return 32 + 32 + 32;
  case kSurfaceFormat32_32_32_32:
    return 32 + 32 + 32 + 32;
  case kSurfaceFormat5_6_5:
    return 5 + 6 + 5;
  case kSurfaceFormat1_5_5_5:
    return 1 + 5 + 5 + 5;
  case kSurfaceFormat5_5_5_1:
    return 5 + 5 + 5 + 1;
  case kSurfaceFormat4_4_4_4:
    return 4 + 4 + 4 + 4;
  case kSurfaceFormat8_24:
    return 8 + 24;
  case kSurfaceFormat24_8:
    return 24 + 8;
  case kSurfaceFormatX24_8_32:
    return 24 + 8 + 32;
  case kSurfaceFormatGB_GR:
    return 2 + 2;
  case kSurfaceFormatBG_RG:
    return 0;
  case kSurfaceFormat5_9_9_9:
    return 5 + 9 + 9 + 9;
  case kSurfaceFormatBc1:
    return 0;
  case kSurfaceFormatBc2:
    return 0;
  case kSurfaceFormatBc3:
    return 32;
  case kSurfaceFormatBc4:
    return 0;
  case kSurfaceFormatBc5:
    return 0;
  case kSurfaceFormatBc6:
    return 0;
  case kSurfaceFormatBc7:
    return 0;
  case kSurfaceFormatFmask8_S2_F1:
    return 0;
  case kSurfaceFormatFmask8_S4_F1:
    return 0;
  case kSurfaceFormatFmask8_S8_F1:
    return 0;
  case kSurfaceFormatFmask8_S2_F2:
    return 0;
  case kSurfaceFormatFmask8_S4_F2:
    return 0;
  case kSurfaceFormatFmask8_S4_F4:
    return 0;
  case kSurfaceFormatFmask16_S16_F1:
    return 0;
  case kSurfaceFormatFmask16_S8_F2:
    return 0;
  case kSurfaceFormatFmask32_S16_F2:
    return 0;
  case kSurfaceFormatFmask32_S8_F4:
    return 0;
  case kSurfaceFormatFmask32_S8_F8:
    return 0;
  case kSurfaceFormatFmask64_S16_F4:
    return 0;
  case kSurfaceFormatFmask64_S16_F8:
    return 0;
  case kSurfaceFormat4_4:
    return 4 + 4;
  case kSurfaceFormat6_5_5:
    return 6 + 5 + 5;
  case kSurfaceFormat1:
    return 1;
  case kSurfaceFormat1Reversed:
    return 0;
  }

  return 0;
}

static VkFormat surfaceFormatToVkFormat(SurfaceFormat surface,
                                        TextureChannelType channel) {
  switch (surface) {
  case kSurfaceFormat32:
    switch (channel) {
    case kTextureChannelTypeUInt:
      return VK_FORMAT_R32_UINT;
    case kTextureChannelTypeSInt:
      return VK_FORMAT_R32_SINT;
    case kTextureChannelTypeFloat:
      return VK_FORMAT_R32_SFLOAT;
    default:
      break;
    }
    break;

  case kSurfaceFormat32_32:
    switch (channel) {
    case kTextureChannelTypeUInt:
      return VK_FORMAT_R32G32_UINT;
    case kTextureChannelTypeSInt:
      return VK_FORMAT_R32G32_SINT;
    case kTextureChannelTypeFloat:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      break;
    }
    break;

  case kSurfaceFormat16_16_16_16:
    switch (channel) {
    case kTextureChannelTypeUNorm:
      return VK_FORMAT_R16G16B16A16_UNORM;
    case kTextureChannelTypeSNorm:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case kTextureChannelTypeUScaled:
      return VK_FORMAT_R16G16B16A16_USCALED;
    case kTextureChannelTypeSScaled:
      return VK_FORMAT_R16G16B16A16_SSCALED;
    case kTextureChannelTypeUInt:
      return VK_FORMAT_R16G16B16A16_UINT;
    case kTextureChannelTypeSInt:
      return VK_FORMAT_R16G16B16A16_SINT;
    case kTextureChannelTypeFloat:
      return VK_FORMAT_R16G16B16A16_SFLOAT;

    default:
      break;
    }
    break;

  case kSurfaceFormat32_32_32:
    switch (channel) {
    case kTextureChannelTypeUInt:
      return VK_FORMAT_R32G32B32_UINT;
    case kTextureChannelTypeSInt:
      return VK_FORMAT_R32G32B32_SINT;
    case kTextureChannelTypeFloat:
      return VK_FORMAT_R32G32B32_SFLOAT;
    default:
      break;
    }
    break;
  case kSurfaceFormat32_32_32_32:
    switch (channel) {
    case kTextureChannelTypeUInt:
      return VK_FORMAT_R32G32B32A32_UINT;
    case kTextureChannelTypeSInt:
      return VK_FORMAT_R32G32B32A32_SINT;
    case kTextureChannelTypeFloat:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:
      break;
    }
    break;

  case kSurfaceFormat8_8_8_8:
    switch (channel) {
    case kTextureChannelTypeUNorm:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case kTextureChannelTypeSNorm:
      return VK_FORMAT_R8G8B8A8_SNORM;
    case kTextureChannelTypeUScaled:
      return VK_FORMAT_R8G8B8A8_USCALED;
    case kTextureChannelTypeSScaled:
      return VK_FORMAT_R8G8B8A8_SSCALED;
    case kTextureChannelTypeUInt:
      return VK_FORMAT_R8G8B8A8_UINT;
    case kTextureChannelTypeSInt:
      return VK_FORMAT_R8G8B8A8_SINT;
    // case kTextureChannelTypeSNormNoZero:
    //   return VK_FORMAT_R8G8B8A8_SNORM;
    case kTextureChannelTypeSrgb:
      return VK_FORMAT_R8G8B8A8_SRGB;
      // case kTextureChannelTypeUBNorm:
      //   return VK_FORMAT_R8G8B8A8_UNORM;
      // case kTextureChannelTypeUBNormNoZero:
      //   return VK_FORMAT_R8G8B8A8_UNORM;
      // case kTextureChannelTypeUBInt:
      //   return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
      // case kTextureChannelTypeUBScaled:
      //   return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;

    default:
      break;
    }
    break;

  case kSurfaceFormatBc1:
    switch (channel) {
    case kTextureChannelTypeSrgb:
      return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    default:
      break;
    }

  case kSurfaceFormatBc3:
    switch (channel) {
    case kTextureChannelTypeSrgb:
      return VK_FORMAT_BC3_SRGB_BLOCK;
    default:
      break;
    }

  case kSurfaceFormatBc4:
    switch (channel) {
    case kTextureChannelTypeUNorm:
      return VK_FORMAT_BC4_UNORM_BLOCK;

    case kTextureChannelTypeSNorm:
      return VK_FORMAT_BC4_SNORM_BLOCK;

    default:
      break;
    }
  case kSurfaceFormatBc5:
    switch (channel) {
    case kTextureChannelTypeUNorm:
      return VK_FORMAT_BC5_UNORM_BLOCK;

    case kTextureChannelTypeSNorm:
      return VK_FORMAT_BC5_SNORM_BLOCK;

    default:
      break;
    }

  case kSurfaceFormatBc7:
    switch (channel) {
    case kTextureChannelTypeUNorm:
      return VK_FORMAT_BC7_UNORM_BLOCK;

    case kTextureChannelTypeSrgb:
      return VK_FORMAT_BC7_SRGB_BLOCK;

    default:
      break;
    }

  default:
    break;
  }

  util::unreachable("unimplemented surface format. %x.%x\n", (int)surface,
                    (int)channel);
}

static VkPrimitiveTopology getVkPrimitiveType(PrimitiveType type) {
  switch (type) {
  case kPrimitiveTypePointList:
    return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  case kPrimitiveTypeLineList:
    return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  case kPrimitiveTypeLineStrip:
    return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  case kPrimitiveTypeTriList:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  case kPrimitiveTypeTriFan:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  case kPrimitiveTypeTriStrip:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  case kPrimitiveTypePatch:
    return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
  case kPrimitiveTypeLineListAdjacency:
    return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
  case kPrimitiveTypeLineStripAdjacency:
    return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
  case kPrimitiveTypeTriListAdjacency:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
  case kPrimitiveTypeTriStripAdjacency:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
  case kPrimitiveTypeLineLoop:
    return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; // FIXME

  case kPrimitiveTypeRectList:
  case kPrimitiveTypeQuadList:
  case kPrimitiveTypeQuadStrip:
  case kPrimitiveTypePolygon:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  default:
    util::unreachable();
  }
}

static std::pair<std::uint64_t, std::uint64_t>
quadListPrimConverter(std::uint64_t index) {
  static constexpr int indecies[] = {0, 1, 2, 2, 3, 0};
  return {index, index / 6 + indecies[index % 6]};
}

static std::pair<std::uint64_t, std::uint64_t>
quadStripPrimConverter(std::uint64_t index) {
  static constexpr int indecies[] = {0, 1, 3, 0, 3, 2};
  return {index, (index / 6) * 4 + indecies[index % 6]};
}

using ConverterFn =
    std::pair<std::uint64_t, std::uint64_t>(std::uint64_t index);

static ConverterFn *getPrimConverterFn(PrimitiveType primType,
                                       std::uint32_t *count) {
  switch (primType) {
  case kPrimitiveTypeQuadList:
    *count = *count / 4 * 6;
    return quadListPrimConverter;

  case kPrimitiveTypeQuadStrip:
    *count = *count / 4 * 6;
    return quadStripPrimConverter;

  default:
    util::unreachable();
  }
}

static bool isPrimRequiresConversion(PrimitiveType primType) {
  switch (primType) {
  case kPrimitiveTypePointList:
  case kPrimitiveTypeLineList:
  case kPrimitiveTypeLineStrip:
  case kPrimitiveTypeTriList:
  case kPrimitiveTypeTriFan:
  case kPrimitiveTypeTriStrip:
  case kPrimitiveTypePatch:
  case kPrimitiveTypeLineListAdjacency:
  case kPrimitiveTypeLineStripAdjacency:
  case kPrimitiveTypeTriListAdjacency:
  case kPrimitiveTypeTriStripAdjacency:
    return false;
  case kPrimitiveTypeLineLoop: // FIXME
    util::unreachable();
    return false;

  case kPrimitiveTypeRectList:
    return false; // handled by geometry shader

  case kPrimitiveTypeQuadList:
  case kPrimitiveTypeQuadStrip:
  case kPrimitiveTypePolygon:
    return true;

  default:
    util::unreachable();
  }
}

static std::uint32_t getPrimDrawCount(PrimitiveType primType,
                                      std::uint32_t count) {
  switch (primType) {
  case kPrimitiveTypePointList:
  case kPrimitiveTypeLineList:
  case kPrimitiveTypeLineStrip:
  case kPrimitiveTypeTriList:
  case kPrimitiveTypeTriFan:
  case kPrimitiveTypeTriStrip:
  case kPrimitiveTypePatch:
  case kPrimitiveTypeLineListAdjacency:
  case kPrimitiveTypeLineStripAdjacency:
  case kPrimitiveTypeTriListAdjacency:
  case kPrimitiveTypeTriStripAdjacency:
  case kPrimitiveTypeRectList: // FIXME
    return count;

  case kPrimitiveTypeLineLoop: // FIXME
    util::unreachable();

  case kPrimitiveTypeQuadList:
    return (count / 4) * 6;

  case kPrimitiveTypeQuadStrip:
    return (count / 2) * 6;

  case kPrimitiveTypePolygon:
    util::unreachable();

  default:
    util::unreachable();
  }
}

static bool validateSpirv(const std::vector<uint32_t> &bin) {
  spv_target_env target_env = SPV_ENV_VULKAN_1_3;
  spv_context spvContext = spvContextCreate(target_env);
  spv_diagnostic diagnostic = nullptr;
  spv_const_binary_t binary = {bin.data(), bin.size()};
  spv_result_t error = spvValidate(spvContext, &binary, &diagnostic);
  if (error != 0)
    spvDiagnosticPrint(diagnostic);
  spvDiagnosticDestroy(diagnostic);
  spvContextDestroy(spvContext);
  return error == 0;
}

static void printSpirv(const std::vector<uint32_t> &bin) {
  // spv_target_env target_env = SPV_ENV_VULKAN_1_3;
  // spv_context spvContext = spvContextCreate(target_env);
  // spv_diagnostic diagnostic = nullptr;

  // spv_result_t error = spvBinaryToText(
  //     spvContext, bin.data(), bin.size(),
  //     SPV_BINARY_TO_TEXT_OPTION_PRINT | // SPV_BINARY_TO_TEXT_OPTION_COLOR |
  //         SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES |
  //         SPV_BINARY_TO_TEXT_OPTION_COMMENT |
  //         SPV_BINARY_TO_TEXT_OPTION_INDENT,
  //     nullptr, &diagnostic);

  // if (error != 0) {
  //   spvDiagnosticPrint(diagnostic);
  // }

  // spvDiagnosticDestroy(diagnostic);
  // spvContextDestroy(spvContext);

  // if (error != 0) {
  //   return;
  // }

  // spirv_cross::CompilerGLSL glsl(bin);
  // spirv_cross::CompilerGLSL::Options options;
  // options.version = 460;
  // options.es = false;
  // options.vulkan_semantics = true;
  // glsl.set_common_options(options);
  // std::printf("%s\n", glsl.compile().c_str());
}

static std::optional<std::vector<uint32_t>>
optimizeSpirv(std::span<const std::uint32_t> spirv) {
  spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
  optimizer.RegisterPerformancePasses();
  optimizer.RegisterPass(spvtools::CreateSimplificationPass());

  std::vector<uint32_t> result;
  if (optimizer.Run(spirv.data(), spirv.size(), &result)) {
    return result;
  }

  util::unreachable();
  return {};
}

static VkShaderStageFlagBits shaderStageToVk(amdgpu::shader::Stage stage) {
  switch (stage) {
  case amdgpu::shader::Stage::None:
    break;
  case amdgpu::shader::Stage::Fragment:
    return VK_SHADER_STAGE_FRAGMENT_BIT;
  case amdgpu::shader::Stage::Vertex:
    return VK_SHADER_STAGE_VERTEX_BIT;
  case amdgpu::shader::Stage::Geometry:
    return VK_SHADER_STAGE_GEOMETRY_BIT;
  case amdgpu::shader::Stage::Compute:
    return VK_SHADER_STAGE_COMPUTE_BIT;
  }

  return VK_SHADER_STAGE_ALL;
}

class DeviceMemory {
  VkDeviceMemory mDeviceMemory = VK_NULL_HANDLE;
  VkDeviceSize mSize = 0;
  unsigned mMemoryTypeIndex = 0;

public:
  DeviceMemory(DeviceMemory &) = delete;
  DeviceMemory(DeviceMemory &&other) { *this = std::move(other); }
  DeviceMemory() = default;

  ~DeviceMemory() {
    if (mDeviceMemory != nullptr) {
      vkFreeMemory(g_vkDevice, mDeviceMemory, g_vkAllocator);
    }
  }

  DeviceMemory &operator=(DeviceMemory &&other) {
    std::swap(mDeviceMemory, other.mDeviceMemory);
    std::swap(mSize, other.mSize);
    std::swap(mMemoryTypeIndex, other.mMemoryTypeIndex);
    return *this;
  }

  VkDeviceMemory getHandle() const { return mDeviceMemory; }
  VkDeviceSize getSize() const { return mSize; }
  unsigned getMemoryTypeIndex() const { return mMemoryTypeIndex; }

  static DeviceMemory AllocateFromType(std::size_t size,
                                       unsigned memoryTypeIndex) {
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    DeviceMemory result;
    Verify() << vkAllocateMemory(g_vkDevice, &allocInfo, g_vkAllocator,
                                 &result.mDeviceMemory);
    result.mSize = size;
    result.mMemoryTypeIndex = memoryTypeIndex;
    return result;
  }

  static DeviceMemory Allocate(std::size_t size, unsigned memoryTypeBits,
                               VkMemoryPropertyFlags properties) {
    return AllocateFromType(
        size, findPhysicalMemoryTypeIndex(memoryTypeBits, properties));
  }

  static DeviceMemory Allocate(VkMemoryRequirements requirements,
                               VkMemoryPropertyFlags properties) {
    return AllocateFromType(
        requirements.size,
        findPhysicalMemoryTypeIndex(requirements.memoryTypeBits, properties));
  }

  static DeviceMemory CreateExternalFd(int fd, std::size_t size,
                                       unsigned memoryTypeIndex) {
    VkImportMemoryFdInfoKHR importMemoryInfo{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        nullptr,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        fd,
    };

    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &importMemoryInfo,
        .allocationSize = size,
        .memoryTypeIndex = memoryTypeIndex,
    };

    DeviceMemory result;
    Verify() << vkAllocateMemory(g_vkDevice, &allocInfo, g_vkAllocator,
                                 &result.mDeviceMemory);
    result.mSize = size;
    result.mMemoryTypeIndex = memoryTypeIndex;
    return result;
  }
  static DeviceMemory
  CreateExternalHostMemory(void *hostPointer, std::size_t size,
                           VkMemoryPropertyFlags properties) {
    VkMemoryHostPointerPropertiesEXT hostPointerProperties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};

    auto vkGetMemoryHostPointerPropertiesEXT =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(
            g_vkDevice, "vkGetMemoryHostPointerPropertiesEXT");

    Verify() << vkGetMemoryHostPointerPropertiesEXT(
        g_vkDevice, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        hostPointer, &hostPointerProperties);

    auto memoryTypeBits = hostPointerProperties.memoryTypeBits;

    VkImportMemoryHostPointerInfoEXT importMemoryInfo = {
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        nullptr,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        hostPointer,
    };

    auto memoryTypeIndex =
        findPhysicalMemoryTypeIndex(memoryTypeBits, properties);

    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &importMemoryInfo,
        .allocationSize = size,
        .memoryTypeIndex = memoryTypeIndex,
    };

    DeviceMemory result;
    Verify() << vkAllocateMemory(g_vkDevice, &allocInfo, g_vkAllocator,
                                 &result.mDeviceMemory);
    result.mSize = size;
    result.mMemoryTypeIndex = memoryTypeIndex;
    return result;
  }

  void *map(VkDeviceSize offset, VkDeviceSize size) {
    void *result = 0;
    Verify() << vkMapMemory(g_vkDevice, mDeviceMemory, offset, size, 0,
                            &result);

    return result;
  }

  void unmap() { vkUnmapMemory(g_vkDevice, mDeviceMemory); }
};

struct DeviceMemoryRef {
  VkDeviceMemory deviceMemory = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
  VkDeviceSize size = 0;
  void *data = nullptr;
};

class MemoryResource {
  DeviceMemory mMemory;
  VkMemoryPropertyFlags mProperties = 0;
  std::size_t mSize = 0;
  std::size_t mAllocationOffset = 0;
  char *mData = nullptr;

public:
  MemoryResource(const MemoryResource &) = delete;

  MemoryResource() = default;
  MemoryResource(MemoryResource &&other) = default;
  MemoryResource &operator=(MemoryResource &&other) = default;

  ~MemoryResource() {
    if (mMemory.getHandle() != nullptr && mData != nullptr) {
      vkUnmapMemory(g_vkDevice, mMemory.getHandle());
    }
  }

  void clear() { mAllocationOffset = 0; }

  static MemoryResource CreateFromFd(int fd, std::size_t size) {
    auto properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    MemoryResource result;
    result.mMemory = DeviceMemory::CreateExternalFd(
        fd, size, findPhysicalMemoryTypeIndex(~0, properties));
    result.mProperties = properties;
    result.mSize = size;

    return result;
  }

  static MemoryResource CreateFromHost(void *data, std::size_t size) {
    auto properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    MemoryResource result;
    result.mMemory =
        DeviceMemory::CreateExternalHostMemory(data, size, properties);
    result.mProperties = properties;
    result.mSize = size;

    return result;
  }

  static MemoryResource CreateHostVisible(std::size_t size) {
    auto properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    MemoryResource result;
    result.mMemory = DeviceMemory::Allocate(size, ~0, properties);
    result.mProperties = properties;
    result.mSize = size;

    void *data = nullptr;
    Verify() << vkMapMemory(g_vkDevice, result.mMemory.getHandle(), 0, size, 0,
                            &data);
    result.mData = reinterpret_cast<char *>(data);

    return result;
  }

  static MemoryResource CreateDeviceLocal(std::size_t size) {
    auto properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    MemoryResource result;
    result.mMemory = DeviceMemory::Allocate(size, ~0, properties);
    result.mProperties = properties;
    result.mSize = size;
    return result;
  }

  DeviceMemoryRef allocate(VkMemoryRequirements requirements) {
    if ((requirements.memoryTypeBits & (1 << mMemory.getMemoryTypeIndex())) ==
        0) {
      util::unreachable();
    }

    auto offset = (mAllocationOffset + requirements.alignment - 1) &
                  ~(requirements.alignment - 1);
    mAllocationOffset = offset + requirements.size;
    if (mAllocationOffset > mSize) {
      util::unreachable("out of memory resource");
    }

    return {mMemory.getHandle(), offset, requirements.size, mData};
  }

  DeviceMemoryRef getFromOffset(std::uint64_t offset, std::size_t size) {
    return {mMemory.getHandle(), offset, size, nullptr};
  }

  std::size_t getSize() const { return mSize; }

  explicit operator bool() const { return mMemory.getHandle() != nullptr; }
};

struct Semaphore {
  VkSemaphore mSemaphore = VK_NULL_HANDLE;

public:
  Semaphore(const Semaphore &) = delete;

  Semaphore() = default;
  Semaphore(Semaphore &&other) { *this = std::move(other); }

  Semaphore &operator=(Semaphore &&other) {
    std::swap(mSemaphore, other.mSemaphore);
    return *this;
  }

  ~Semaphore() {
    if (mSemaphore != VK_NULL_HANDLE) {
      vkDestroySemaphore(g_vkDevice, mSemaphore, nullptr);
    }
  }

  static Semaphore Create(std::uint64_t initialValue = 0) {
    VkSemaphoreTypeCreateInfo typeCreateInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr,
        VK_SEMAPHORE_TYPE_TIMELINE, initialValue};

    VkSemaphoreCreateInfo createInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                        &typeCreateInfo, 0};

    Semaphore result;
    Verify() << vkCreateSemaphore(g_vkDevice, &createInfo, nullptr,
                                  &result.mSemaphore);
    return result;
  }

  VkResult wait(std::uint64_t value, uint64_t timeout) const {
    VkSemaphoreWaitInfo waitInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                    nullptr,
                                    VK_SEMAPHORE_WAIT_ANY_BIT,
                                    1,
                                    &mSemaphore,
                                    &value};

    return vkWaitSemaphores(g_vkDevice, &waitInfo, timeout);
  }

  void signal(std::uint64_t value) {
    VkSemaphoreSignalInfo signalInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                        nullptr, mSemaphore, value};

    Verify() << vkSignalSemaphore(g_vkDevice, &signalInfo);
  }

  std::uint64_t getCounterValue() const {
    std::uint64_t result = 0;
    Verify() << vkGetSemaphoreCounterValue(g_vkDevice, mSemaphore, &result);
    return result;
  }

  VkSemaphore getHandle() const { return mSemaphore; }

  bool operator==(std::nullptr_t) const { return mSemaphore == nullptr; }
  bool operator!=(std::nullptr_t) const { return mSemaphore != nullptr; }
};

struct CommandBuffer {
  VkCommandBuffer mCmdBuffer = VK_NULL_HANDLE;

public:
  CommandBuffer(const CommandBuffer &) = delete;

  CommandBuffer() = default;
  CommandBuffer(CommandBuffer &&other) { *this = std::move(other); }

  CommandBuffer &operator=(CommandBuffer &&other) {
    std::swap(mCmdBuffer, other.mCmdBuffer);
    return *this;
  }

  CommandBuffer(VkCommandPool commandPool,
                VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                VkCommandBufferUsageFlagBits flags = {}) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = level;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(g_vkDevice, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = flags;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
  }

  void end() { vkEndCommandBuffer(mCmdBuffer); }

  bool operator==(std::nullptr_t) const { return mCmdBuffer == nullptr; }
  bool operator!=(std::nullptr_t) const { return mCmdBuffer != nullptr; }
};

class Buffer {
  VkBuffer mBuffer = VK_NULL_HANDLE;
  DeviceMemoryRef mMemory;

public:
  Buffer(const Buffer &) = delete;

  Buffer() = default;
  Buffer(Buffer &&other) { *this = std::move(other); }
  ~Buffer() {
    if (mBuffer != nullptr) {
      vkDestroyBuffer(g_vkDevice, mBuffer, g_vkAllocator);
    }
  }

  Buffer &operator=(Buffer &&other) {
    std::swap(mBuffer, other.mBuffer);
    std::swap(mMemory, other.mMemory);
    return *this;
  }

  Buffer(std::size_t size, VkBufferUsageFlags usage,
         VkBufferCreateFlags flags = 0,
         VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         std::span<const std::uint32_t> queueFamilyIndices = {}) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.flags = flags;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = sharingMode;
    bufferInfo.queueFamilyIndexCount = queueFamilyIndices.size();
    bufferInfo.pQueueFamilyIndices = queueFamilyIndices.data();

    Verify() << vkCreateBuffer(g_vkDevice, &bufferInfo, g_vkAllocator,
                               &mBuffer);
  }

  void *getData() const {
    return reinterpret_cast<char *>(mMemory.data) + mMemory.offset;
  }

  static Buffer
  CreateExternal(std::size_t size, VkBufferUsageFlags usage,
                 VkBufferCreateFlags flags = 0,
                 VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                 std::span<const std::uint32_t> queueFamilyIndices = {}) {
    VkExternalMemoryBufferCreateInfo info{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.pNext = &info;
    bufferInfo.flags = flags;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = sharingMode;
    bufferInfo.queueFamilyIndexCount = queueFamilyIndices.size();
    bufferInfo.pQueueFamilyIndices = queueFamilyIndices.data();

    Buffer result;

    Verify() << vkCreateBuffer(g_vkDevice, &bufferInfo, g_vkAllocator,
                               &result.mBuffer);

    return result;
  }

  static Buffer
  Allocate(MemoryResource &pool, std::size_t size, VkBufferUsageFlags usage,
           VkBufferCreateFlags flags = 0,
           VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE,
           std::span<const std::uint32_t> queueFamilyIndices = {}) {
    Buffer result(size, usage, flags, sharingMode, queueFamilyIndices);
    result.allocateAndBind(pool);

    return result;
  }

  VkBuffer getHandle() const { return mBuffer; }
  [[nodiscard]] VkBuffer release() { return std::exchange(mBuffer, nullptr); }

  VkMemoryRequirements getMemoryRequirements() const {
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(g_vkDevice, mBuffer, &requirements);
    return requirements;
  }

  void allocateAndBind(MemoryResource &pool) {
    auto memory = pool.allocate(getMemoryRequirements());
    bindMemory(memory);
  }

  void bindMemory(DeviceMemoryRef memory) {
    Verify() << vkBindBufferMemory(g_vkDevice, mBuffer, memory.deviceMemory,
                                   memory.offset);
    mMemory = memory;
  }

  void copyTo(VkCommandBuffer cmdBuffer, VkBuffer dstBuffer,
              std::span<const VkBufferCopy> regions) {
    vkCmdCopyBuffer(cmdBuffer, mBuffer, dstBuffer, regions.size(),
                    regions.data());

    VkDependencyInfo depInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    vkCmdPipelineBarrier2(cmdBuffer, &depInfo);
  }

  void readFromImage(const void *address, SurfaceFormat format, int tileMode,
                     uint32_t width, uint32_t height, uint32_t depth) {
    auto pixelSize = (getBitWidthOfSurfaceFormat(format) + 7) / 8;
    if (pixelSize == 0) {
      pixelSize = 4;
    }
    auto imageSize = width * height * depth * pixelSize;

    if (false && tileMode == 0xa) {
      auto src = reinterpret_cast<const char *>(address);
      auto dst = reinterpret_cast<char *>(getData());

      int tilerIndex = surfaceTiler::precalculateTiles(width, height);

      for (std::uint64_t y = 0; y < height; ++y) {
        for (std::uint64_t x = 0; x < width; ++x) {
          std::memcpy(
              dst + (x + y * width) * pixelSize,
              src + surfaceTiler::getTiledElementByteOffset(tilerIndex, x, y),
              pixelSize);
        }
      }
    } else if (getData() != nullptr && tileMode != 0) {
      if (tileMode != 8) {
        std::fprintf(stderr, "Unsupported tile mode %x\n", tileMode);
      }

      std::memcpy(getData(), address, imageSize);
    }
  }

  void writeAsImageTo(void *address, SurfaceFormat format, int tileMode,
                      uint32_t width, uint32_t height, uint32_t depth) {
    auto pixelSize = (getBitWidthOfSurfaceFormat(format) + 7) / 8;

    if (pixelSize == 0) {
      pixelSize = 4;
    }

    auto bufferSize = width * height * depth * pixelSize;

    if (false && tileMode == 0xa) {
      auto dst = reinterpret_cast<char *>(address);
      auto src = reinterpret_cast<const char *>(getData());

      int tilerIndex = surfaceTiler::precalculateTiles(width, height);

      for (std::uint64_t y = 0; y < height; ++y) {
        for (std::uint64_t x = 0; x < width; ++x) {
          std::memcpy(
              dst + surfaceTiler::getTiledElementByteOffset(tilerIndex, x, y),
              src + (x + y * width) * pixelSize, pixelSize);
        }
      }
    } else if (address != nullptr && tileMode != 0) {
      if (tileMode != 8) {
        std::fprintf(stderr, "Unsupported tile mode %x\n", tileMode);
      }
      std::memcpy(address, getData(), bufferSize);
    }
  }

  // const DeviceMemoryRef &getMemory() const { return mMemory; }
  bool operator==(std::nullptr_t) const { return mBuffer == nullptr; }
  bool operator!=(std::nullptr_t) const { return mBuffer != nullptr; }
};

class Image2D {
  VkImage mImage = VK_NULL_HANDLE;
  VkFormat mFormat = {};
  VkImageAspectFlags mAspects = {};
  VkImageLayout mLayout = {};
  unsigned mWidth = 0;
  unsigned mHeight = 0;
  DeviceMemoryRef mMemory;

public:
  Image2D(const Image2D &) = delete;

  Image2D() = default;
  Image2D(Image2D &&other) { *this = std::move(other); }

  ~Image2D() {
    if (mImage != nullptr && mMemory.deviceMemory != VK_NULL_HANDLE) {
      vkDestroyImage(g_vkDevice, mImage, g_vkAllocator);
    }
  }

  Image2D &operator=(Image2D &&other) {
    std::swap(mImage, other.mImage);
    std::swap(mFormat, other.mFormat);
    std::swap(mAspects, other.mAspects);
    std::swap(mLayout, other.mLayout);
    std::swap(mWidth, other.mWidth);
    std::swap(mHeight, other.mHeight);
    std::swap(mMemory, other.mMemory);
    return *this;
  }

  Image2D(uint32_t width, uint32_t height, VkFormat format,
          VkImageUsageFlags usage,
          VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL,
          VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
          VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE,
          uint32_t mipLevels = 1, uint32_t arrayLevels = 1,
          VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = arrayLevels;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = samples;
    imageInfo.sharingMode = sharingMode;

    mFormat = format;

    if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      mAspects |= VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    } else {
      mAspects |= VK_IMAGE_ASPECT_COLOR_BIT;
    }

    mLayout = initialLayout;
    mWidth = width;
    mHeight = height;

    Verify() << vkCreateImage(g_vkDevice, &imageInfo, nullptr, &mImage);
  }

  static Image2D
  Allocate(MemoryResource &pool, uint32_t width, uint32_t height,
           VkFormat format, VkImageUsageFlags usage,
           VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL,
           VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
           VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE,
           uint32_t mipLevels = 1, uint32_t arrayLevels = 1,
           VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED) {

    Image2D result(width, height, format, usage, tiling, samples, sharingMode,
                   mipLevels, arrayLevels, initialLayout);

    result.allocateAndBind(pool);
    return result;
  }

  static Image2D CreateFromExternal(VkImage image, VkFormat format,
                                    VkImageAspectFlags aspects,
                                    VkImageLayout layout, unsigned width,
                                    unsigned height) {
    Image2D result;
    result.mImage = image;
    result.mFormat = format;
    result.mAspects = aspects;
    result.mLayout = layout;
    result.mWidth = width;
    result.mHeight = height;
    return result;
  }

  VkImage getHandle() const { return mImage; }
  [[nodiscard]] VkImage release() { return std::exchange(mImage, nullptr); }

  VkMemoryRequirements getMemoryRequirements() const {
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_vkDevice, mImage, &requirements);
    return requirements;
  }

  void allocateAndBind(MemoryResource &pool) {
    auto memory = pool.allocate(getMemoryRequirements());
    bindMemory(memory);
  }

  void bindMemory(DeviceMemoryRef memory) {
    mMemory = memory;
    Verify() << vkBindImageMemory(g_vkDevice, mImage, memory.deviceMemory,
                                  memory.offset);
  }

  void readFromBuffer(VkCommandBuffer cmdBuffer, const Buffer &buffer,
                      VkImageAspectFlags destAspect) {
    transitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    copyBufferToImage(cmdBuffer, mImage, buffer.getHandle(), mWidth, mHeight, 0,
                      0, 0, destAspect);
  }

  void writeToBuffer(VkCommandBuffer cmdBuffer, const Buffer &buffer,
                     VkImageAspectFlags sourceAspect) {
    transitionLayout(cmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    copyImageToBuffer(cmdBuffer, mImage, buffer.getHandle(), mWidth, mHeight, 0,
                      0, 0, sourceAspect);
  }

  [[nodiscard]] Buffer writeToBuffer(VkCommandBuffer cmdBuffer,
                                     MemoryResource &pool,
                                     VkImageAspectFlags sourceAspect) {
    auto transferBuffer = Buffer::Allocate(
        pool, mWidth * mHeight * 4,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    writeToBuffer(cmdBuffer, transferBuffer, sourceAspect);
    return transferBuffer;
  }

  [[nodiscard]] Buffer read(VkCommandBuffer cmdBuffer, MemoryResource &pool,
                            const void *address, int tileMode,
                            VkImageAspectFlags destAspect) {
    auto transferBuffer = Buffer::Allocate(
        pool, mWidth * mHeight * 4,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    transferBuffer.readFromImage(address,
                                 SurfaceFormat::kSurfaceFormat8_8_8_8, // TODO
                                 tileMode, mWidth, mHeight, 1);

    readFromBuffer(cmdBuffer, transferBuffer, destAspect);

    return transferBuffer;
  }

  void transitionLayout(VkCommandBuffer cmdBuffer, VkImageLayout newLayout) {
    if (mLayout == newLayout) {
      return;
    }

    transitionImageLayout(cmdBuffer, mImage, mAspects, mLayout, newLayout);

    mLayout = newLayout;
  }

  // DeviceMemoryRef getMemory() const { return mMemory; }
};

struct DirectMemory {
  MemoryResource memoryResource;
  Buffer buffer;
};

static std::map<std::uint64_t, DirectMemory, std::greater<>> directMemory;

static DirectMemory &getDirectMemory(std::uint64_t address, std::size_t size,
                                     std::uint64_t *beginAddress = nullptr) {
  auto it = directMemory.lower_bound(address);

  if (it == directMemory.end() ||
      it->second.memoryResource.getSize() < address) {
    auto zone = memoryZoneTable.queryZone(address / kPageSize);
    zone.beginAddress *= kPageSize;
    zone.endAddress *= kPageSize;

    assert(address >= zone.beginAddress && address + size < zone.endAddress);

    auto newResource = MemoryResource::CreateFromHost(
        (char *)g_rwMemory + zone.beginAddress - g_memoryBase,
        zone.endAddress - zone.beginAddress);

    it = directMemory.emplace_hint(
        it, zone.beginAddress,
        DirectMemory{.memoryResource = std::move(newResource)});
  }

  if (beginAddress != nullptr) {
    *beginAddress = it->first;
  }
  return it->second;
}

struct BufferRef {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
  VkDeviceSize size = 0;
};

static BufferRef getDirectBuffer(std::uint64_t address, std::size_t size) {
  std::uint64_t beginAddress;
  auto &dm = getDirectMemory(address, size, &beginAddress);

  if (dm.buffer == nullptr) {
    dm.buffer = Buffer::CreateExternal(dm.memoryResource.getSize(),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    dm.buffer.bindMemory(
        dm.memoryResource.getFromOffset(0, dm.memoryResource.getSize()));
  }

  return {dm.buffer.getHandle(), address - beginAddress, size};
}

void updateDirectState() {
  auto zones = std::move(memoryZoneTable.invalidatedZones);
  memoryZoneTable.invalidatedZones = {};
  auto it = directMemory.begin();

  if (it == directMemory.end()) {
    return;
  }

  for (auto zone : zones) {
    while (it->first > zone) {
      if (++it == directMemory.end()) {
        return;
      }
    }

    if (it->first == zone) {
      it = directMemory.erase(it);

      if (it == directMemory.end()) {
        return;
      }
    }
  }
}

MemoryResource hostVisibleMemory;
MemoryResource deviceLocalMemory;

static MemoryResource &getHostVisibleMemory() {
  if (!hostVisibleMemory) {
    hostVisibleMemory = MemoryResource::CreateHostVisible(1024 * 1024 * 512);
  }

  return hostVisibleMemory;
}

static MemoryResource &getDeviceLocalMemory() {
  if (!deviceLocalMemory) {
    deviceLocalMemory = MemoryResource::CreateDeviceLocal(1024 * 1024 * 512);
  }

  return deviceLocalMemory;
}

struct RenderState {
  DrawContext &ctxt;
  amdgpu::RemoteMemory memory;

  struct StoreUniformInfo {
    std::uint64_t dstAddress;
    std::uint64_t size;
    void *source;
  };

  StoreUniformInfo storeUniforms[16];
  std::size_t storeUniformCount = 0;

  std::vector<VkDeviceMemory> usedMemory;

  std::vector<VkVertexInputBindingDescription> vertexBindings;
  std::vector<VkVertexInputAttributeDescription> vertexAttrs;
  std::vector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings;
  std::vector<VkWriteDescriptorSet> writeDescriptorSets;
  std::vector<VkDescriptorBufferInfo> descriptorBufferInfos;
  std::vector<VkDescriptorImageInfo> descriptorImageInfos;
  std::vector<Image2D> images;
  std::vector<Buffer> buffers;
  std::vector<VkImageView> imageViews;
  std::vector<VkSampler> samplers;

  ~RenderState() {
    for (auto view : imageViews) {
      vkDestroyImageView(g_vkDevice, view, nullptr);
    }

    images.clear();

    for (auto sampler : samplers) {
      vkDestroySampler(g_vkDevice, sampler, nullptr);
    }

    for (auto memory : usedMemory) {
      vkFreeMemory(g_vkDevice, memory, nullptr);
    }
  }

  struct StorageBuffer {
    BufferRef buffer;
    void *indirectMemory;
  };

  StorageBuffer getStorageBuffer(std::uint64_t address, std::size_t size) {
    if (kUseDirectMemory &&
        (address &
         (g_physicalDeviceProperties.limits.minStorageBufferOffsetAlignment -
          1)) == 0) {
      // offset is supported natively, return direct buffer
      return {getDirectBuffer(address, size), nullptr};
    }

    // create temporary buffer
    auto tmpBuffer = Buffer::Allocate(getHostVisibleMemory(), size,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(tmpBuffer.getData(), memory.getPointer(address), size);

    auto result =
        BufferRef{.buffer = tmpBuffer.getHandle(), .offset = 0, .size = size};
    buffers.push_back(std::move(tmpBuffer));
    return {result, tmpBuffer.getData()};
  }

  BufferRef getIndexBuffer(std::uint64_t address, std::size_t size) {
    if (kUseDirectMemory &&
        (address &
         (g_physicalDeviceProperties.limits.minStorageBufferOffsetAlignment -
          1)) == 0) {
      // offset is supported natively, return direct buffer
      return getDirectBuffer(address, size);
    }

    // create temporary buffer
    auto tmpBuffer = Buffer::Allocate(getHostVisibleMemory(), size,
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    std::memcpy(tmpBuffer.getData(), memory.getPointer(address), size);

    auto result =
        BufferRef{.buffer = tmpBuffer.getHandle(), .offset = 0, .size = size};
    buffers.push_back(std::move(tmpBuffer));
    return result;
  }

  std::vector<std::uint32_t> loadShader(
      VkCommandBuffer cmdBuffer, shader::Stage stage, std::uint64_t address,
      std::uint32_t *userSgprs, std::size_t userSgprsCount, int &bindingOffset,
      std::uint32_t dimX = 1, std::uint32_t dimY = 1, std::uint32_t dimZ = 1) {
    auto shader = shader::convert(
        memory, stage, address,
        std::span<const std::uint32_t>(userSgprs, userSgprsCount),
        bindingOffset, dimX, dimY, dimZ);

    if (!validateSpirv(shader.spirv)) {
      printSpirv(shader.spirv);
      dumpShader(memory.getPointer<std::uint32_t>(address));
      util::unreachable();
    }

    if (auto opt = optimizeSpirv(shader.spirv)) {
      shader.spirv = std::move(*opt);
    }
    // printSpirv(shader.spirv);

    // if (stage == shader::Stage::Compute) {
    // dumpShader(memory.getPointer<std::uint32_t>(address));
    // printSpirv(shader.spirv);
    // }

    bindingOffset += shader.uniforms.size();

    auto vkStage = shaderStageToVk(stage);

    descriptorBufferInfos.reserve(64);
    descriptorImageInfos.reserve(64);

    for (auto &uniform : shader.uniforms) {
      VkDescriptorType descriptorType;
      switch (uniform.kind) {
      case shader::Shader::UniformKind::Buffer: {
        descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

        auto vbuffer = reinterpret_cast<GnmVBuffer *>(uniform.buffer);
        auto size = vbuffer->getSize();
        if (size == 0) {
          size = 0x10;
        }

        auto [storageBuffer, indirectMemory] =
            getStorageBuffer(vbuffer->getAddress(), size);

        descriptorBufferInfos.push_back(descriptorBufferInfo(
            storageBuffer.buffer, storageBuffer.offset, size));

        writeDescriptorSets.push_back(
            writeDescriptorSetBuffer(nullptr, descriptorType, uniform.binding,
                                     &descriptorBufferInfos.back(), 1));

        if (indirectMemory != nullptr &&
            (uniform.accessOp & amdgpu::shader::AccessOp::Store) ==
                amdgpu::shader::AccessOp::Store) {
          if (storeUniformCount >= std::size(storeUniforms)) {
            std::fprintf(stderr, "Too many uniform stores\n");
            std::abort();
          }

          storeUniforms[storeUniformCount++] = {.dstAddress =
                                                    vbuffer->getAddress(),
                                                .size = size,
                                                .source = indirectMemory};
        }
        break;
      }

      case shader::Shader::UniformKind::Image: {
        descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;

        auto tbuffer = reinterpret_cast<GnmTBuffer *>(uniform.buffer);
        auto dataFormat = tbuffer->dfmt;
        auto channelType = tbuffer->nfmt;
        auto colorFormat = surfaceFormatToVkFormat(dataFormat, channelType);
        std::printf("tbuffer address = %lx (%lx), width=%u, "
                    "height=%u,pitch=%u,type=%u,tiling_idx=%u\n",
                    tbuffer->getAddress(), tbuffer->baseaddr256, tbuffer->width,
                    tbuffer->height, tbuffer->pitch, (unsigned)tbuffer->type,
                    tbuffer->tiling_idx);
        std::fflush(stdout);

        assert(tbuffer->width == tbuffer->pitch);

        auto image = Image2D::Allocate(
            getDeviceLocalMemory(), tbuffer->width + 1, tbuffer->height + 1,
            colorFormat,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

        buffers.push_back(image.read(cmdBuffer, getHostVisibleMemory(),
                                     memory.getPointer(tbuffer->getAddress()),
                                     tbuffer->tiling_idx,
                                     VK_IMAGE_ASPECT_COLOR_BIT));

        auto imageView =
            createImageView2D(image.getHandle(), colorFormat, {},
                              imageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT));
        imageViews.push_back(imageView);

        descriptorImageInfos.push_back(
            descriptorImageInfo(VK_NULL_HANDLE, imageView,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

        writeDescriptorSets.push_back(
            writeDescriptorSetImage(nullptr, descriptorType, uniform.binding,
                                    &descriptorImageInfos.back(), 1));

        image.transitionLayout(cmdBuffer,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        images.push_back(std::move(image));
        break;
      }

      case shader::Shader::UniformKind::Sampler: {
        VkSamplerCreateInfo samplerInfo{};
        // TODO: load S# sampler
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.compareOp = VK_COMPARE_OP_NEVER;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        samplerInfo.maxAnisotropy = 1.0;
        samplerInfo.anisotropyEnable = VK_FALSE;

        VkSampler sampler;
        Verify() << vkCreateSampler(g_vkDevice, &samplerInfo, nullptr,
                                    &sampler);
        samplers.push_back(sampler);

        descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;

        descriptorImageInfos.push_back(descriptorImageInfo(
            sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

        writeDescriptorSets.push_back(
            writeDescriptorSetImage(nullptr, descriptorType, uniform.binding,
                                    &descriptorImageInfos.back(), 1));
        break;
      }
      }

      descriptorSetLayoutBindings.push_back(createDescriptorSetLayoutBinding(
          uniform.binding, 1, descriptorType, vkStage));
    }

    return std::move(shader.spirv);
  }

  void uploadUniforms() {
    for (auto uniform : std::span(storeUniforms, storeUniformCount)) {
      std::memcpy(memory.getPointer(uniform.dstAddress), uniform.source,
                  uniform.size);
    }

    storeUniformCount = 0;
  }

  void eliminateFastClear() {
    // TODO
    // util::unreachable();
  }

  void resolve() {
    // TODO: when texture cache will be implemented it MSAA should be done by
    // GPU
    auto srcBuffer = colorBuffers[0];
    auto dstBuffer = colorBuffers[1];

    const auto src = memory.getPointer(srcBuffer.base);
    auto dst = memory.getPointer(dstBuffer.base);

    if (src == nullptr || dst == nullptr) {
      return;
    }

    std::memcpy(dst, src, screenScissorH * screenScissorW * 4);
  }

  void draw(std::uint32_t count, std::uint64_t indeciesAddress,
            std::uint32_t indexCount) {
    if (cbColorFormat == CbColorFormat::Disable) {
      return;
    }

    if (cbColorFormat == CbColorFormat::EliminateFastClear) {
      eliminateFastClear();
      return;
    }

    if (cbColorFormat == CbColorFormat::Resolve) {
      resolve();
      return;
    }

    if (pgmVsAddress == 0 || pgmPsAddress == 0) {
      return;
    }

    if (cbRenderTargetMask == 0 || colorBuffers[0].base == 0) {
      return;
    }

    updateDirectState();

    getHostVisibleMemory().clear();
    getDeviceLocalMemory().clear();

    depthClearEnable = true;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkCommandBuffer transferCommandBuffers[2];

    {
      VkCommandBufferAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocInfo.commandPool = ctxt.commandPool;
      allocInfo.commandBufferCount = std::size(transferCommandBuffers);

      Verify() << vkAllocateCommandBuffers(g_vkDevice, &allocInfo,
                                           transferCommandBuffers);
    }

    VkCommandBuffer readCommandBuffer = transferCommandBuffers[0];
    Verify() << vkBeginCommandBuffer(readCommandBuffer, &beginInfo);

    auto primType = static_cast<PrimitiveType>(vgtPrimitiveType);

    int bindingOffset = 0;
    auto vertexShader =
        loadShader(readCommandBuffer, shader::Stage::Vertex, pgmVsAddress,
                   userVsData, vsUserSpgrs, bindingOffset);
    auto fragmentShader =
        loadShader(readCommandBuffer, shader::Stage::Fragment, pgmPsAddress,
                   userPsData, psUserSpgrs, bindingOffset);

    auto colorFormat = VK_FORMAT_R8G8B8A8_SRGB;      // TODO
    auto depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT; // TODO

    std::vector<Image2D> colorImages;
    std::vector<VkImageView> framebufferAttachments;

    for (auto targetMask = cbRenderTargetMask;
         auto &colorBuffer : colorBuffers) {
      if (targetMask == 0 || colorBuffer.base == 0) {
        break;
      }

      if ((targetMask & 0xf) == 0) {
        targetMask >>= 4;
        continue;
      }

      targetMask >>= 4;

      // TODO: implement MRT
      if (!colorImages.empty()) {
        std::printf("MRT!\n");
        break;
      }

      auto colorImage = Image2D::Allocate(
          getDeviceLocalMemory(), screenScissorW, screenScissorH, colorFormat,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
              VK_IMAGE_USAGE_TRANSFER_DST_BIT);
      buffers.push_back(colorImage.read(
          readCommandBuffer, getHostVisibleMemory(),
          memory.getPointer(colorBuffer.base), colorBuffer.tileModeIndex,
          VK_IMAGE_ASPECT_COLOR_BIT));

      colorImage.transitionLayout(readCommandBuffer,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

      auto colorImageView =
          createImageView2D(colorImage.getHandle(), colorFormat, {},
                            imageSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT));

      colorImages.push_back(std::move(colorImage));
      framebufferAttachments.push_back(colorImageView);
    }

    auto depthImage = Image2D::Allocate(
        getDeviceLocalMemory(), screenScissorW, screenScissorH, depthFormat,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            (depthClearEnable || zReadBase == 0
                 ? 0
                 : VK_IMAGE_USAGE_TRANSFER_DST_BIT));

    if (!depthClearEnable && zReadBase) {
      buffers.push_back(depthImage.read(
          readCommandBuffer, getHostVisibleMemory(),
          memory.getPointer(zReadBase), 8, VK_IMAGE_ASPECT_DEPTH_BIT));
    }

    depthImage.transitionLayout(
        readCommandBuffer, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    auto depthImageView =
        createImageView2D(depthImage.getHandle(), depthFormat, {},
                          imageSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                VK_IMAGE_ASPECT_STENCIL_BIT));

    auto renderPass = createRenderPass(colorFormat, depthFormat);

    framebufferAttachments.push_back(depthImageView);
    auto framebuffer = createFramebuffer(
        renderPass, {screenScissorW, screenScissorH}, framebufferAttachments);

    ShaderModule shader{};

    shader.descriptorSetLayout =
        createDescriptorSetLayout(descriptorSetLayoutBindings);

    shader.descriptorPool =
        createDescriptorPool(64, std::array{createDescriptorPoolSize(
                                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64)});

    auto descriptorSet = createDescriptorSet(&shader);

    for (auto &writeSet : writeDescriptorSets) {
      writeSet.dstSet = descriptorSet;
    }

    updateDescriptorSets(writeDescriptorSets);

    shader.pipelineLayout = createPipelineLayout(shader.descriptorSetLayout);

    std::vector<VkPipelineShaderStageCreateInfo> shaders;

    shaders.push_back(createPipelineShaderStage(ctxt, vertexShader,
                                                VK_SHADER_STAGE_VERTEX_BIT));

    if (primType == kPrimitiveTypeRectList) {
      shaders.push_back(createPipelineShaderStage(
          ctxt, spirv_rect_list_geom, VK_SHADER_STAGE_GEOMETRY_BIT));
    }

    shaders.push_back(createPipelineShaderStage(ctxt, fragmentShader,
                                                VK_SHADER_STAGE_FRAGMENT_BIT));

    shader.pipeline = createGraphicsPipeline(
        shader.pipelineLayout, renderPass, ctxt.pipelineCache,
        createPipelineVertexInputState(vertexBindings, vertexAttrs),
        getVkPrimitiveType(primType), shaders);

    VkCommandBuffer drawCommandBuffer;
    {
      VkCommandBufferAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocInfo.commandPool = ctxt.commandPool;
      allocInfo.commandBufferCount = 1;

      Verify() << vkAllocateCommandBuffers(g_vkDevice, &allocInfo,
                                           &drawCommandBuffer);
    }

    Verify() << vkBeginCommandBuffer(drawCommandBuffer, &beginInfo);

    VkClearValue clearValues[2];
    clearValues[0].color = {{1.f, 1.f, 1.f, 1.0f}};
    clearValues[1].depthStencil = {depthClear, 0};

    VkRenderPassBeginInfo renderPassBeginInfo{};
    renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBeginInfo.renderPass = renderPass;
    renderPassBeginInfo.framebuffer = framebuffer;
    renderPassBeginInfo.renderArea.extent = {screenScissorW, screenScissorH};
    renderPassBeginInfo.clearValueCount = 2;
    renderPassBeginInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(drawCommandBuffer, &renderPassBeginInfo,
                         VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(drawCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      shader.pipeline);

    VkViewport viewport{};
    viewport.x = screenScissorX;
    viewport.y = (float)screenScissorH - screenScissorY;
    viewport.width = screenScissorW;
    viewport.height = -(float)screenScissorH;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(drawCommandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent.width = screenScissorW;
    scissor.extent.height = screenScissorH;
    scissor.offset.x = screenScissorX;
    scissor.offset.y = screenScissorY;
    vkCmdSetScissor(drawCommandBuffer, 0, 1, &scissor);

    vkCmdBindDescriptorSets(drawCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            shader.pipelineLayout, 0, 1, &descriptorSet, 0,
                            nullptr);

    Buffer indexBufferStorage;
    BufferRef indexBuffer;
    auto needConversion = isPrimRequiresConversion(primType);
    VkIndexType vkIndexType =
        (indexType & 0x1f) == 0 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

    if (needConversion) {
      auto indecies = memory.getPointer(indeciesAddress);
      if (indecies == nullptr) {
        indexCount = count;
      }

      unsigned origIndexSize = vkIndexType == VK_INDEX_TYPE_UINT16 ? 16 : 32;
      auto converterFn = getPrimConverterFn(primType, &indexCount);

      if (indecies == nullptr) {
        if (indexCount < 0x10000) {
          vkIndexType = VK_INDEX_TYPE_UINT16;
        } else if (indecies) {
          vkIndexType = VK_INDEX_TYPE_UINT32;
        }
      }

      unsigned indexSize = vkIndexType == VK_INDEX_TYPE_UINT16 ? 16 : 32;
      auto indexBufferSize = indexSize * indexCount;

      indexBufferStorage = Buffer::Allocate(
          getHostVisibleMemory(), indexBufferSize,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

      void *data = indexBufferStorage.getData();

      if (indecies == nullptr) {
        if (indexSize == 16) {
          for (std::uint32_t i = 0; i < indexCount; ++i) {
            auto [dstIndex, srcIndex] = converterFn(i);
            ((std::uint16_t *)data)[dstIndex] = srcIndex;
          }
        } else {
          for (std::uint32_t i = 0; i < indexCount; ++i) {
            auto [dstIndex, srcIndex] = converterFn(i);
            ((std::uint32_t *)data)[dstIndex] = srcIndex;
          }
        }
      } else {
        if (indexSize == 16) {
          for (std::uint32_t i = 0; i < indexCount; ++i) {
            auto [dstIndex, srcIndex] = converterFn(i);
            std::uint32_t origIndex =
                origIndexSize == 16 ? ((std::uint16_t *)indecies)[srcIndex]
                                    : ((std::uint32_t *)indecies)[srcIndex];
            ((std::uint16_t *)data)[dstIndex] = origIndex;
          }

        } else {
          for (std::uint32_t i = 0; i < indexCount; ++i) {
            auto [dstIndex, srcIndex] = converterFn(i);
            std::uint32_t origIndex =
                origIndexSize == 16 ? ((std::uint16_t *)indecies)[srcIndex]
                                    : ((std::uint32_t *)indecies)[srcIndex];
            ((std::uint32_t *)data)[dstIndex] = origIndex;
          }
        }
      }

      indexBuffer = {indexBufferStorage.getHandle(), 0, indexBufferSize};
    } else if (indeciesAddress != 0) {
      unsigned indexSize = vkIndexType == VK_INDEX_TYPE_UINT16 ? 16 : 32;
      auto indexBufferSize = indexSize * indexCount;
      indexBuffer = getIndexBuffer(indeciesAddress, indexBufferSize);
    }

    if (indexBuffer.buffer == nullptr) {
      vkCmdDraw(drawCommandBuffer, count, 1, 0, 0);
    } else {
      vkCmdBindIndexBuffer(drawCommandBuffer, indexBuffer.buffer,
                           indexBuffer.offset, vkIndexType);
      vkCmdDrawIndexed(drawCommandBuffer, indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(drawCommandBuffer);
    vkEndCommandBuffer(drawCommandBuffer);

    VkSemaphore readCompleteSem;
    VkSemaphore drawCompleteSem;
    VkFence drawCompleteFence;
    VkFence writeCompleteFence;
    {
      VkSemaphoreCreateInfo semCreateInfo{
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      };
      vkCreateSemaphore(g_vkDevice, &semCreateInfo, g_vkAllocator,
                        &readCompleteSem);
      vkCreateSemaphore(g_vkDevice, &semCreateInfo, g_vkAllocator,
                        &drawCompleteSem);

      VkFenceCreateInfo fenceCreateInfo{
          .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      vkCreateFence(g_vkDevice, &fenceCreateInfo, g_vkAllocator,
                    &drawCompleteFence);
      vkCreateFence(g_vkDevice, &fenceCreateInfo, g_vkAllocator,
                    &writeCompleteFence);
    }

    VkPipelineStageFlags drawDstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkPipelineStageFlags writeDstStageMask =
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    {
      VkSubmitInfo submitInfo{};
      submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submitInfo.commandBufferCount = 1;
      submitInfo.pCommandBuffers = &readCommandBuffer;
      submitInfo.pSignalSemaphores = &readCompleteSem;
      submitInfo.signalSemaphoreCount = 1;

      vkEndCommandBuffer(readCommandBuffer);
      Verify() << vkQueueSubmit(ctxt.queue, 1, &submitInfo, VK_NULL_HANDLE);
    }

    {
      VkSubmitInfo submitInfo{};
      submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submitInfo.commandBufferCount = 1;
      submitInfo.pCommandBuffers = &drawCommandBuffer;
      submitInfo.pWaitSemaphores = &readCompleteSem;
      submitInfo.waitSemaphoreCount = 1;
      submitInfo.pSignalSemaphores = &drawCompleteSem;
      submitInfo.signalSemaphoreCount = 1;
      submitInfo.pWaitDstStageMask = &drawDstStageMask;
      Verify() << vkQueueSubmit(ctxt.queue, 1, &submitInfo, drawCompleteFence);
    }

    vkWaitForFences(g_vkDevice, 1, &drawCompleteFence, VK_TRUE, UINT64_MAX);

    VkCommandBuffer writeCommandBuffer = transferCommandBuffers[1];
    Verify() << vkBeginCommandBuffer(writeCommandBuffer, &beginInfo);

    std::vector<Buffer> resultColorBuffers;
    resultColorBuffers.reserve(colorImages.size());

    for (auto &colorImage : colorImages) {
      resultColorBuffers.push_back(
          colorImage.writeToBuffer(writeCommandBuffer, getHostVisibleMemory(),
                                   VK_IMAGE_ASPECT_COLOR_BIT));
    }

    // TODO: implement mrt support

    Buffer resultDepthBuffer;
    if (depthWriteEnable && zWriteBase != 0) {
      resultDepthBuffer =
          depthImage.writeToBuffer(writeCommandBuffer, getHostVisibleMemory(),
                                   VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    {
      VkSubmitInfo submitInfo{};
      submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submitInfo.commandBufferCount = 1;
      submitInfo.pCommandBuffers = &writeCommandBuffer;
      submitInfo.pWaitSemaphores = &drawCompleteSem;
      submitInfo.waitSemaphoreCount = 1;
      submitInfo.pWaitDstStageMask = &writeDstStageMask;
      vkEndCommandBuffer(writeCommandBuffer);
      Verify() << vkQueueSubmit(ctxt.queue, 1, &submitInfo, writeCompleteFence);
    }

    vkWaitForFences(g_vkDevice, 1, &writeCompleteFence, VK_TRUE, UINT64_MAX);

    // TODO: make image read/write on gpu side
    for (std::size_t i = 0, end = resultColorBuffers.size(); i < end; ++i) {
      auto &colorBuffer = colorBuffers[i];

      resultColorBuffers[i].writeAsImageTo(
          memory.getPointer(colorBuffer.base), kSurfaceFormat8_8_8_8,
          colorBuffer.tileModeIndex, screenScissorW, screenScissorH, 1);

      std::printf("Writing color to %lx\n", colorBuffer.base);
    }

    if (depthWriteEnable && zWriteBase != 0) {
      resultDepthBuffer.writeAsImageTo(memory.getPointer(zWriteBase),
                                       kSurfaceFormat32, 8, screenScissorW,
                                       screenScissorH, 1);
    }

    uploadUniforms();

    shader.destroy();

    vkDestroySemaphore(g_vkDevice, readCompleteSem, g_vkAllocator);
    vkDestroySemaphore(g_vkDevice, drawCompleteSem, g_vkAllocator);
    vkDestroyFence(g_vkDevice, drawCompleteFence, g_vkAllocator);
    vkDestroyFence(g_vkDevice, writeCompleteFence, g_vkAllocator);

    vkDestroyFramebuffer(g_vkDevice, framebuffer, nullptr);
    vkDestroyRenderPass(g_vkDevice, renderPass, nullptr);

    for (auto attachment : framebufferAttachments) {
      vkDestroyImageView(g_vkDevice, attachment, nullptr);
    }
  }

  void dispatch(std::size_t dimX, std::size_t dimY, std::size_t dimZ) {
    getHostVisibleMemory().clear();
    getDeviceLocalMemory().clear();
    updateDirectState();

    int bindingOffset = 0;

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = ctxt.commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(g_vkDevice, &allocInfo, &commandBuffer);
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    auto computeShader =
        loadShader(commandBuffer, shader::Stage::Compute, pgmComputeAddress,
                   userComputeData, computeUserSpgrs, bindingOffset,
                   computeNumThreadX, computeNumThreadY, computeNumThreadZ);
    ShaderModule shader{};

    shader.descriptorSetLayout =
        createDescriptorSetLayout(descriptorSetLayoutBindings);

    shader.descriptorPool =
        createDescriptorPool(64, std::array{createDescriptorPoolSize(
                                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64)});

    auto descriptorSet = createDescriptorSet(&shader);

    for (auto &writeSet : writeDescriptorSets) {
      writeSet.dstSet = descriptorSet;
    }

    updateDescriptorSets(writeDescriptorSets);

    shader.pipelineLayout = createPipelineLayout(shader.descriptorSetLayout);

    shader.pipeline = createComputePipeline(
        shader.pipelineLayout, ctxt.pipelineCache,
        createPipelineShaderStage(ctxt, computeShader,
                                  VK_SHADER_STAGE_COMPUTE_BIT));
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      shader.pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            shader.pipelineLayout, 0, 1, &descriptorSet, 0,
                            nullptr);
    vkCmdDispatch(commandBuffer, dimX, dimY, dimZ);
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    Verify() << vkQueueSubmit(ctxt.queue, 1, &submitInfo, nullptr);
    Verify() << vkQueueWaitIdle(ctxt.queue);

    uploadUniforms();

    shader.destroy();
  }
};

void amdgpu::device::draw(RemoteMemory memory, DrawContext &ctxt,
                          std::uint32_t count, std::uint64_t indeciesAddress,
                          std::uint32_t indexCount) {
  RenderState{.ctxt = ctxt, .memory = memory}.draw(count, indeciesAddress,
                                                   indexCount);
}

void amdgpu::device::dispatch(RemoteMemory memory, DrawContext &ctxt,
                              std::size_t dimX, std::size_t dimY,
                              std::size_t dimZ) {

  RenderState{.ctxt = ctxt, .memory = memory}.dispatch(dimX, dimY, dimZ);
}

enum class EventWriteSource : std::uint8_t {
  Immediate32 = 0x1,
  Immediate64 = 0x2,
  GlobalClockCounter = 0x3,
  GpuCoreClockCounter = 0x4,
};

struct EopData {
  std::uint32_t eventType;
  std::uint32_t eventIndex;
  std::uint64_t address;
  std::uint64_t value;
  std::uint8_t dstSel;
  std::uint8_t intSel;
  EventWriteSource eventSource;
};

static std::uint64_t globalClock() {
  // TODO
  return 0x0;
}

static std::uint64_t gpuCoreClock() {
  // TODO
  return 0x0;
}

static void writeEop(amdgpu::RemoteMemory memory, EopData data) {
  switch (data.eventSource) {
  case EventWriteSource::Immediate32: {
    *memory.getPointer<std::uint32_t>(data.address) = data.value;
    break;
  }
  case EventWriteSource::Immediate64: {
    *memory.getPointer<std::uint64_t>(data.address) = data.value;
    break;
  }
  case EventWriteSource::GlobalClockCounter: {
    *memory.getPointer<std::uint64_t>(data.address) = globalClock();
    break;
  }
  case EventWriteSource::GpuCoreClockCounter: {
    *memory.getPointer<std::uint64_t>(data.address) = gpuCoreClock();
    break;
  }
  }
}

static void drawIndexAuto(amdgpu::RemoteMemory memory,
                          amdgpu::device::DrawContext &ctxt,
                          std::uint32_t count) {
  draw(memory, ctxt, count, 0, 0);
}

static void drawIndex2(amdgpu::RemoteMemory memory,
                       amdgpu::device::DrawContext &ctxt, std::uint32_t maxSize,
                       std::uint64_t address, std::uint32_t count) {
  draw(memory, ctxt, count, address, maxSize);
}

void amdgpu::device::handleCommandBuffer(RemoteMemory memory, DrawContext &ctxt,
                                         std::uint32_t *cmds,
                                         std::uint32_t count) {
  bool log = false;
  for (std::uint32_t cmdOffset = 0; cmdOffset < count; ++cmdOffset) {
    auto cmd = cmds[cmdOffset];
    auto type = getBits(cmd, 31, 30);

    if (type == 0) {
      std::printf("!packet type 0!\n");
      auto baseIndex = getBits(cmd, 15, 0);
      auto count = getBits(cmd, 29, 16);
      std::printf("-- %04x: %08x: baseIndex=%x, count=%d\n", cmdOffset, cmd,
                  baseIndex, count);
      cmdOffset += count;
    } else if (type == 1) {
      std::printf("Unexpected packet type 1!\n");
    } else if (type == 2) {
      std::printf("!packet type 2!\n");
      continue;
    } else if (type == 3) {
      auto predicate = getBit(cmd, 0);
      auto shaderType = getBit(cmd, 1);
      auto op = getBits(cmd, 15, 8);
      auto len = getBits(cmd, 29, 16) + 1;

      if (log) {
        std::printf("-- %04x: %08x: %s len=%d, shaderType = %cX\n", cmdOffset,
                    cmd, opcodeToString(op).c_str(), len,
                    'G' ^ (shaderType << 1));

        for (std::uint32_t offset = 0; offset < len; ++offset) {
          std::printf("   %04x: %08x\n", cmdOffset + 1 + offset,
                      cmds[cmdOffset + 1 + offset]);
        }
      }

      switch (op) {
      case kOpcodeLOAD_CONST_RAM: {
        std::uint64_t addressLo = cmds[cmdOffset + 1];
        std::uint64_t addressHi = cmds[cmdOffset + 2];
        std::uint32_t numDw = getBits(cmds[cmdOffset + 3], 14, 0);
        std::uint32_t offset = getBits(cmds[cmdOffset + 4], 15, 0);
        if (log) {
          std::printf("   ` address=%lx, numDw = %x, offset=%x\n",
                      addressLo | (addressHi << 32), numDw, offset);
        }
        break;
      }

      case kOpcodeSET_UCONFIG_REG: {
        std::uint32_t baseRegOffset = 0xc000 + cmds[cmdOffset + 1];

        for (std::uint32_t regOffset = 0; regOffset < len - 1; ++regOffset) {
          if (log) {
            std::printf("   %04x: %04x: %s = 0x%08x\n",
                        cmdOffset + 2 + regOffset,
                        (baseRegOffset + regOffset) << 2,
                        registerToString(baseRegOffset + regOffset).c_str(),
                        cmds[cmdOffset + 2 + regOffset]);
          }

          setRegister(baseRegOffset + regOffset,
                      cmds[cmdOffset + 2 + regOffset]);
        }
        break;
      }

      case kOpcodeSET_CONTEXT_REG: {
        std::uint32_t baseRegOffset = 0xa000 + cmds[cmdOffset + 1];

        for (std::uint32_t regOffset = 0; regOffset < len - 1; ++regOffset) {
          if (log) {
            std::printf("   %04x: %04x: %s = 0x%08x\n",
                        cmdOffset + 2 + regOffset,
                        (baseRegOffset + regOffset) << 2,
                        registerToString(baseRegOffset + regOffset).c_str(),
                        cmds[cmdOffset + 2 + regOffset]);
          }
          setRegister(baseRegOffset + regOffset,
                      cmds[cmdOffset + 2 + regOffset]);
        }
        break;
      }

      case kOpcodeSET_SH_REG: {
        std::uint32_t baseRegOffset = 0x2c00 + cmds[cmdOffset + 1];

        for (std::uint32_t regOffset = 0; regOffset < len - 1; ++regOffset) {
          if (log) {
            std::printf("   %04x: %04x: %s = 0x%08x\n",
                        cmdOffset + 2 + regOffset,
                        (baseRegOffset + regOffset) << 2,
                        registerToString(baseRegOffset + regOffset).c_str(),
                        cmds[cmdOffset + 2 + regOffset]);
          }

          setRegister(baseRegOffset + regOffset,
                      cmds[cmdOffset + 2 + regOffset]);
        }
        break;
      }

      case kOpcodeWRITE_DATA: {
        auto control = cmds[cmdOffset + 1];
        auto destAddrLo = cmds[cmdOffset + 2];
        auto destAddrHi = cmds[cmdOffset + 3];
        auto data = cmds + cmdOffset + 4;
        auto size = len - 3;

        // 0 - Micro Engine - ME
        // 1 - Prefetch parser - PFP
        // 2 - Constant engine - CE
        // 3 - Dispatch engine - DE
        auto engineSel = getBits(control, 31, 30);

        // wait for confirmation that write complete
        auto wrConfirm = getBit(control, 20);

        // do not increment address
        auto wrOneAddr = getBit(control, 16);

        // 0 - mem-mapped register
        // 1 - memory sync
        // 2 - tc/l2
        // 3 - gds
        // 4 - reserved
        // 5 - memory async
        auto dstSel = getBits(control, 11, 8);

        auto memMappedRegisterAddress = getBits(destAddrLo, 15, 0);
        auto memory32bit = getBits(destAddrLo, 31, 2);
        auto memory64bit = getBits(destAddrLo, 31, 3);
        auto gdsOffset = getBits(destAddrLo, 15, 0);

        if (log) {
          std::printf("   %04x: control=%x [engineSel=%d, "
                      "wrConfirm=%d,wrOneAddr=%d, dstSel=%d]\n",
                      cmdOffset + 1, control, engineSel, wrConfirm, wrOneAddr,
                      dstSel);

          std::printf("   %04x: destAddrLo=%x "
                      "[memory32bit=%x,memory64bit=%x,gdsOffset=%x]\n",
                      cmdOffset + 2, destAddrLo, memory32bit, memory64bit,
                      gdsOffset);

          std::printf("   %04x: destAddrHi=%x\n", cmdOffset + 3, destAddrHi);

          for (std::uint32_t offset = 4; offset < len; ++offset) {
            std::printf("   %04x: %08x\n", cmdOffset + offset,
                        cmds[cmdOffset + offset]);
          }
        }
        auto address =
            destAddrLo | (static_cast<std::uint64_t>(destAddrHi) << 32);
        auto dest = memory.getPointer<std::uint32_t>(address);
        if (log) {
          std::printf("   address=%lx\n", address);
        }
        for (unsigned i = 0; i < size; ++i) {
          dest[i] = data[i];
        }

        break;
      }

      case kOpcodeINDEX_TYPE: {
        indexType = cmds[cmdOffset + 1];
        break;
      }

      case kOpcodeDRAW_INDEX_AUTO: {
        drawIndexAuto(memory, ctxt, cmds[cmdOffset + 1]);
        break;
      }

      case kOpcodeDRAW_INDEX_2: {
        auto maxSize = cmds[cmdOffset + 1];
        auto address = cmds[cmdOffset + 2] |
                       (static_cast<std::uint64_t>(cmds[cmdOffset + 3]) << 32);
        auto count = cmds[cmdOffset + 4];
        drawIndex2(memory, ctxt, maxSize, address, count);
        break;
      }

      case kOpcodeDISPATCH_DIRECT: {
        auto dimX = cmds[cmdOffset + 1];
        auto dimY = cmds[cmdOffset + 2];
        auto dimZ = cmds[cmdOffset + 3];
        if (log) {
          std::printf("   %04x: DIM X=%u\n", cmdOffset + 1, dimX);
          std::printf("   %04x: DIM Y=%u\n", cmdOffset + 2, dimY);
          std::printf("   %04x: DIM Z=%u\n", cmdOffset + 3, dimZ);
        }
        dispatch(memory, ctxt, dimX, dimY, dimZ);
      }

      case kOpcodeEVENT_WRITE_EOP: {
        EopData eopData{};
        eopData.eventType = getBits(cmds[cmdOffset + 1], 6, 0);
        eopData.eventIndex = getBits(cmds[cmdOffset + 1], 12, 8);
        eopData.address =
            cmds[cmdOffset + 2] |
            (static_cast<std::uint64_t>(getBits(cmds[cmdOffset + 3], 16, 0))
             << 32);
        eopData.value = cmds[cmdOffset + 4] |
                        (static_cast<std::uint64_t>(cmds[cmdOffset + 5]) << 32);
        eopData.dstSel = 0;
        eopData.intSel = getBits(cmds[cmdOffset + 3], 26, 24);
        eopData.eventSource =
            static_cast<EventWriteSource>(getBits(cmds[cmdOffset + 3], 32, 29));
        writeEop(memory, eopData);
        break;
      }

      case kOpcodeEVENT_WRITE_EOS: {
        std::uint32_t eventType = getBits(cmds[cmdOffset + 1], 6, 0);
        std::uint32_t eventIndex = getBits(cmds[cmdOffset + 1], 12, 8);
        std::uint64_t address =
            cmds[cmdOffset + 2] |
            (static_cast<std::uint64_t>(getBits(cmds[cmdOffset + 3], 16, 0))
             << 32);
        std::uint32_t command = getBits(cmds[cmdOffset + 3], 32, 16);

        if (log) {
          std::printf("address = %#lx\n", address);
          std::printf("command = %#x\n", command);
        }
        if (command == 0x4000) { // store 32bit data
          *memory.getPointer<std::uint32_t>(address) = cmds[cmdOffset + 4];
        } else {
          util::unreachable();
        }

        break;
      }

      case kOpcodeWAIT_REG_MEM: {
        auto function = cmds[cmdOffset + 1] & 7;
        auto pollAddressLo = cmds[cmdOffset + 2];
        auto pollAddressHi = cmds[cmdOffset + 3];
        auto reference = cmds[cmdOffset + 4];
        auto mask = cmds[cmdOffset + 5];
        auto pollInterval = cmds[cmdOffset + 5];

        auto pollAddress =
            pollAddressLo | (static_cast<std::uint64_t>(pollAddressHi) << 32);
        auto pointer = memory.getPointer<volatile std::uint32_t>(pollAddress);

        reference &= mask;

        auto compare = [&](std::uint32_t value, std::uint32_t reference,
                           int function) {
          switch (function) {
          case 0:
            return true;
          case 1:
            return value < reference;
          case 2:
            return value <= reference;
          case 3:
            return value == reference;
          case 4:
            return value != reference;
          case 5:
            return value >= reference;
          case 6:
            return value > reference;
          }

          util::unreachable();
        };

        if (log) {
          std::printf("   polling address %lx, reference = %x, function = %u\n",
                      pollAddress, reference, function);
          std::fflush(stdout);
        }
        while (true) {
          auto value = *pointer & mask;

          if (compare(value, reference, function)) {
            break;
          }
        }
        break;
      }

      case kOpcodeNOP:
        if (log) {
          for (std::uint32_t offset = 0; offset < len; ++offset) {
            std::printf("   %04x: %08x\n", cmdOffset + 1 + offset,
                        cmds[cmdOffset + 1 + offset]);
          }
        }
        break;
      default:
        for (std::uint32_t offset = 0; offset < len; ++offset) {
          std::printf("   %04x: %08x\n", cmdOffset + 1 + offset,
                      cmds[cmdOffset + 1 + offset]);
        }
        break;
      }

      cmdOffset += len;
    }
  }
}

void amdgpu::device::AmdgpuDevice::handleProtectMemory(std::uint64_t address,
                                                       std::uint64_t size,
                                                       std::uint32_t prot) {
  ::mprotect(memory.getPointer(address), size, prot >> 4);

  auto beginPage = address / kPageSize;
  auto endPage = (address + size + kPageSize - 1) / kPageSize;

  if (prot >> 4) {
    memoryZoneTable.map(beginPage, endPage);
    std::printf("Allocated area at %zx, size %zu\n", address, size);
  } else {
    memoryZoneTable.unmap(beginPage, endPage);
  }
}
void amdgpu::device::AmdgpuDevice::handleCommandBuffer(std::uint64_t address,
                                                       std::uint64_t size) {
  auto count = size / sizeof(std::uint32_t);

  std::printf("address = %lx, count = %lx\n", address, count);

  amdgpu::device::handleCommandBuffer(
      memory, dc, memory.getPointer<std::uint32_t>(address), count);
}

bool amdgpu::device::AmdgpuDevice::handleFlip(
    std::uint32_t bufferIndex, std::uint64_t arg, VkCommandBuffer cmd,
    VkImage targetImage, VkExtent2D targetExtent,
    std::vector<VkBuffer> &usedBuffers, std::vector<VkImage> &usedImages) {
  std::printf("requested flip %d\n", bufferIndex);

  bridge->flipBuffer = bufferIndex;
  bridge->flipArg = arg;
  bridge->flipCount = bridge->flipCount + 1;

  auto buffer = bridge->buffers[bufferIndex];

  if (bufferIndex == ~static_cast<std::uint32_t>(0)) {
    // black surface, ignore for now
    return false;
  }

  if (buffer.pitch == 0 || buffer.height == 0 || buffer.address == 0) {
    std::printf("Attempt to flip unallocated buffer\n");
    return false;
  }

  getHostVisibleMemory().clear();
  getDeviceLocalMemory().clear();

  std::printf("flip: address=%lx, buffer=%ux%u, target=%ux%u\n", buffer.address,
              buffer.width, buffer.height, targetExtent.width,
              targetExtent.height);

  auto bufferImage = Image2D::Allocate(getDeviceLocalMemory(), buffer.width,
                                       buffer.height, VK_FORMAT_B8G8R8A8_UNORM,
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT);

  auto tmpBuffer = bufferImage.read(
      cmd, getHostVisibleMemory(), memory.getPointer(buffer.address),
      buffer.tilingMode, VK_IMAGE_ASPECT_COLOR_BIT);
  bufferImage.transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

  transitionImageLayout(cmd, targetImage, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  VkImageBlit region{
      .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                         .mipLevel = 0,
                         .baseArrayLayer = 0,
                         .layerCount = 1},
      .srcOffsets = {{},
                     {static_cast<int32_t>(buffer.width),
                      static_cast<int32_t>(buffer.height), 1}},
      .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                         .mipLevel = 0,
                         .baseArrayLayer = 0,
                         .layerCount = 1},
      .dstOffsets = {{},
                     {static_cast<int32_t>(targetExtent.width),
                      static_cast<int32_t>(targetExtent.height), 1}},
  };

  vkCmdBlitImage(cmd, bufferImage.getHandle(),
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targetImage,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                 VK_FILTER_LINEAR);

  transitionImageLayout(cmd, targetImage, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

  usedBuffers.push_back(tmpBuffer.release());
  usedImages.push_back(bufferImage.release());
  return true;
}
