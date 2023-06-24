#pragma once

namespace amdgpu {
enum PM4Opcodes {
  NOP = 0x10,
  SET_BASE = 0x11,
  CLEAR_STATE = 0x12,
  INDEX_BUFFER_SIZE = 0x13,
  DISPATCH_DIRECT = 0x15,
  DISPATCH_INDIRECT = 0x16,
  INDIRECT_BUFFER_END = 0x17,
  MODE_CONTROL = 0x18,
  ATOMIC_GDS = 0x1D,
  ATOMIC_MEM = 0x1E,
  OCCLUSION_QUERY = 0x1F,
  SET_PREDICATION = 0x20,
  REG_RMW = 0x21,
  COND_EXEC = 0x22,
  PRED_EXEC = 0x23,
  DRAW_INDIRECT = 0x24,
  DRAW_INDEX_INDIRECT = 0x25,
  INDEX_BASE = 0x26,
  DRAW_INDEX_2 = 0x27,
  CONTEXT_CONTROL = 0x28,
  DRAW_INDEX_OFFSET = 0x29,
  INDEX_TYPE = 0x2A,
  DRAW_INDEX = 0x2B,
  DRAW_INDIRECT_MULTI = 0x2C,
  DRAW_INDEX_AUTO = 0x2D,
  DRAW_INDEX_IMMD = 0x2E,
  NUM_INSTANCES = 0x2F,
  DRAW_INDEX_MULTI_AUTO = 0x30,
  INDIRECT_BUFFER_32 = 0x32,
  INDIRECT_BUFFER_CONST = 0x33,
  STRMOUT_BUFFER_UPDATE = 0x34,
  DRAW_INDEX_OFFSET_2 = 0x35,
  DRAW_PREAMBLE = 0x36,
  WRITE_DATA = 0x37,
  DRAW_INDEX_INDIRECT_MULTI = 0x38,
  MEM_SEMAPHORE = 0x39,
  MPEG_INDEX = 0x3A,
  COPY_DW = 0x3B,
  WAIT_REG_MEM = 0x3C,
  MEM_WRITE = 0x3D,
  INDIRECT_BUFFER_3F = 0x3F,
  COPY_DATA = 0x40,
  CP_DMA = 0x41,
  PFP_SYNC_ME = 0x42,
  SURFACE_SYNC = 0x43,
  ME_INITIALIZE = 0x44,
  COND_WRITE = 0x45,
  EVENT_WRITE = 0x46,
  EVENT_WRITE_EOP = 0x47,
  EVENT_WRITE_EOS = 0x48,
  RELEASE_MEM = 0x49,
  PREAMBLE_CNTL = 0x4A,
  RB_OFFSET = 0x4B,
  ALU_PS_CONST_BUFFER_COPY = 0x4C,
  ALU_VS_CONST_BUFFER_COPY = 0x4D,
  ALU_PS_CONST_UPDATE = 0x4E,
  ALU_VS_CONST_UPDATE = 0x4F,
  DMA_DATA = 0x50,
  ONE_REG_WRITE = 0x57,
  AQUIRE_MEM = 0x58,
  REWIND = 0x59,
  LOAD_UCONFIG_REG = 0x5E,
  LOAD_SH_REG = 0x5F,
  LOAD_CONFIG_REG = 0x60,
  LOAD_CONTEXT_REG = 0x61,
  SET_CONFIG_REG = 0x68,
  SET_CONTEXT_REG = 0x69,
  SET_ALU_CONST = 0x6A,
  SET_BOOL_CONST = 0x6B,
  SET_LOOP_CONST = 0x6C,
  SET_RESOURCE = 0x6D,
  SET_SAMPLER = 0x6E,
  SET_CTL_CONST = 0x6F,
  SET_RESOURCE_OFFSET = 0x70,
  SET_ALU_CONST_VS = 0x71,
  SET_ALU_CONST_DI = 0x72,
  SET_CONTEXT_REG_INDIRECT = 0x73,
  SET_RESOURCE_INDIRECT = 0x74,
  SET_APPEND_CNT = 0x75,
  SET_SH_REG = 0x76,
  SET_SH_REG_OFFSET = 0x77,
  SET_QUEUE_REG = 0x78,
  SET_UCONFIG_REG = 0x79,
  SCRATCH_RAM_WRITE = 0x7D,
  SCRATCH_RAM_READ = 0x7E,
  LOAD_CONST_RAM = 0x80,
  WRITE_CONST_RAM = 0x81,
  DUMP_CONST_RAM = 0x83,
  INCREMENT_CE_COUNTER = 0x84,
  INCREMENT_DE_COUNTER = 0x85,
  WAIT_ON_CE_COUNTER = 0x86,
  WAIT_ON_DE_COUNTER_DIFF = 0x88,
  SWITCH_BUFFER = 0x8B,
};

const char *pm4OpcodeToString(int opcode);
} // namespace amdgpu::device

