extern "C" {
#include "amdgpu.h"
#include "amdgpu_drm.h"
#include "xf86drm.h"
}

#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <math.h>
#include <mutex>
#include <sstream>
#include <stdio.h>
#include <string.h>
#include <vector>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#include "sid.h"
#include "gfx9d.h"
#include "sid_tables.h"

#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[1;32m"
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_CYAN "\033[1;36m"

#define INDENT_PKT 8

static void *(*real_dlsym)(void *, const char *) = NULL;

extern "C" void *_dl_sym(void *, const char *, void *);
static void *(*get_real_dlsym())(void *, const char *);

static std::mutex global_mutex;
static std::string output_dir;

struct Buffer_info {
  void *data = nullptr;
};

struct Map_info {
  amdgpu_bo_handle bo;
  std::uint64_t addr;
  std::uint64_t size;
  std::uint64_t offset;
};

static std::map<amdgpu_bo_handle, Buffer_info> buffers;
static std::map<std::uint64_t, Map_info> maps;

void *get_ptr(uint64_t addr, uint64_t size) {
  std::unique_lock<std::mutex> lock(global_mutex);
  auto it = maps.upper_bound(addr);
  if (it == maps.begin()) {
    fprintf(stderr, "map not found %llx\n", addr);
    return nullptr;
  }
  --it;
  if (it == maps.end()) {
    fprintf(stderr, "map not found %llx\n", addr);
    return nullptr;
  }
  if (it->second.addr > addr ||
      it->second.addr + it->second.size < addr + size) {
    fprintf(stderr, "map too small\n");
    return nullptr;
  }
  auto buf_it = buffers.find(it->second.bo);
  if (buf_it == buffers.end()) {
    fprintf(stderr, "could not find buffer associated with map\n");
    return nullptr;
  }
  if (!buf_it->second.data) {
    lock.unlock();
    amdgpu_bo_cpu_map(buf_it->first, &buf_it->second.data);
    lock.lock();
  }

  return (char *)buf_it->second.data + it->second.offset +
         (addr - it->second.addr);
}

static void print_spaces(std::ostream &os, unsigned num) {
  for (unsigned i = 0; i < num; ++i)
    os << ' ';
}

float uif(uint32_t v) {
  float f;
  memcpy(&f, &v, 4);
  return f;
}

uint32_t fui(float f) {
  uint32_t ret;
  memcpy(&ret, &f, 4);
  return ret;
}

static void print_value(std::ostream &os, uint32_t value, int bits) {
  /* Guess if it's int or float */
  if (value <= (1 << 15)) {
    if (value <= 9)
      os << value << "\n";
    else
      os << value << " (0x" << std::hex << std::setw(bits / 4)
         << std::setfill('0') << value << std::dec << ")\n";
  } else {
    float f = uif(value);

    if (fabs(f) < 100000 && f * 10 == floor(f * 10)) {
      os.precision(1);
      os.setf(std::ios::fixed);
      os << f << " (0x" << std::hex << std::setw(bits / 4) << std::setfill('0')
         << value << std::dec << ")\n";
    } else
      /* Don't print more leading zeros than there are bits. */
      os << "0x" << std::hex << std::setw(bits / 4) << std::setfill('0')
         << value << std::dec << "\n";
  }
}

static void si_dump_reg(std::ostream &os, unsigned offset, uint32_t value,
                        uint32_t field_mask) {
  int r, f;

  for (r = 0; r < ARRAY_SIZE(sid_reg_table); r++) {
    const struct si_reg *reg = &sid_reg_table[r];

    if (reg->offset == offset) {
      bool first_field = true;
      const char *reg_name = sid_strings + reg->name_offset;

      print_spaces(os, INDENT_PKT);
      os << COLOR_YELLOW << reg_name << "," << std::hex << value << std::dec << " " << COLOR_RESET << " <- ";

      if (!reg->num_fields) {
        print_value(os, value, 32);
        return;
      }

      for (f = 0; f < reg->num_fields; f++) {
        const struct si_field *field = sid_fields_table + reg->fields_offset + f;
        const int *values_offsets = sid_strings_offsets + field->values_offset;
        uint32_t val = (value & field->mask) >> (ffs(field->mask) - 1);

        if (!(field->mask & field_mask))
          continue;

        /* Indent the field. */
        if (!first_field)
          print_spaces(os, INDENT_PKT + strlen(reg_name) + 4);

        /* Print the field. */
        os << sid_strings + field->name_offset << " = ";

        if (val < field->num_values && values_offsets[val] >= 0)
          os << sid_strings + values_offsets[val] << "\n";
        else
          print_value(os, val, __builtin_popcountll(field->mask));

        first_field = false;
      }
      return;
    }
  }
}

static void print_named_value(std::ostream &os, const char *name,
                              uint32_t value, int bits) {
  print_spaces(os, INDENT_PKT);
  os << COLOR_YELLOW << name << COLOR_RESET " <- ";
  print_value(os, value, bits);
}

static void print_reg_name(std::ostream &os, int offset)
{
    int r, f;

  for (r = 0; r < ARRAY_SIZE(sid_reg_table); r++) {
    const struct si_reg *reg = &sid_reg_table[r];
    const char *reg_name = sid_strings + reg->name_offset;

    if (reg->offset == offset) {
      print_spaces(os, INDENT_PKT);
      os << COLOR_YELLOW << reg_name << COLOR_RESET << "\n";
    }
  }
}

static std::map<std::vector<std::uint32_t>, std::string> ls_shaders, hs_shaders,
    vs_shaders, ps_shaders, gs_shaders, es_shaders, cs_shaders;

std::string
dump_shader(std::map<std::vector<std::uint32_t>, std::string> &cache,
            std::string const &cat, std::uint64_t addr) {
  uint32_t *data = (uint32_t *)get_ptr(addr, 0);
  if (!data) {
    return "unknown shader";
  }

  uint32_t *end = data;
  while (*end != 0xBF810000) {
    ++end;
  }
  ++end;
  std::vector<uint32_t> body(data, end);
  auto it = cache.find(body);
  if (it != cache.end())
    return it->second;

  auto id = cache.size();
  std::string name = cat + "_shader_" + std::to_string(id);
  std::ofstream dump("/tmp/shader_binary");
  dump.write((char *)data, (end - data) * 4);
  dump.close();
  std::string cmd_line =
      "clrxdisasm -r -g Tonga /tmp/shader_binary > " + output_dir + name + ".s";
  if (system(cmd_line.c_str())) {
    std::cerr << "failed to execute clrxdisasm" << std::endl;
  }
  cache[body] = name;
  return name;
}

static std::uint32_t config_reg;
void process_set_reg_mask(std::ostream &os, std::uint32_t reg, std::uint32_t value, std::uint32_t mask) {
  reg &= 0xFFFFFFU;
  si_dump_reg(os, reg, value, mask);
}

void process_set_reg(std::ostream &os, std::uint32_t reg, std::uint32_t value) {
  reg &= 0xFFFFFFU;
  si_dump_reg(os, reg, value, 0xFFFFFFFFU);
  if (reg == R_00B420_SPI_SHADER_PGM_LO_HS && value) {
    auto s = dump_shader(hs_shaders, "hs", static_cast<uint64_t>(value) << 8);
    print_spaces(os, 8);
    os << s << "\n";
  }
  if (reg == R_00B020_SPI_SHADER_PGM_LO_PS && value) {
    auto s = dump_shader(ps_shaders, "ps", static_cast<uint64_t>(value) << 8);
    print_spaces(os, 8);
    os << s << "\n";
  }
  if (reg == R_00B120_SPI_SHADER_PGM_LO_VS && value) {
    auto s = dump_shader(vs_shaders, "vs", static_cast<uint64_t>(value) << 8);
    print_spaces(os, 8);
    os << s << "\n";
  }
  if (reg == R_00B320_SPI_SHADER_PGM_LO_ES && value) {
    auto s = dump_shader(es_shaders, "es", static_cast<uint64_t>(value) << 8);
    print_spaces(os, 8);
    os << s << "\n";
  }
  if (reg == R_00B220_SPI_SHADER_PGM_LO_GS && value) {
    auto s = dump_shader(gs_shaders, "gs", static_cast<uint64_t>(value) << 8);
    print_spaces(os, 8);
    os << s << "\n";
  }
  if (reg == R_00B520_SPI_SHADER_PGM_LO_LS && value) {
    auto s = dump_shader(ls_shaders, "ls", static_cast<uint64_t>(value) << 8);
    print_spaces(os, 8);
    os << s << "\n";
  }
  if (reg == R_00B830_COMPUTE_PGM_LO && value) {
    auto s = dump_shader(cs_shaders, "cs", static_cast<uint64_t>(value) << 8);
    print_spaces(os, 8);
    os << s << "\n";
  }
  if (reg == R_00B450_SPI_SHADER_USER_DATA_HS_8)
    config_reg = value;
}

void process_packet0(std::ostream &os, uint32_t const *packet) {
  unsigned reg = PKT0_BASE_INDEX_G(*packet) * 4;
  unsigned cnt = PKT_COUNT_G(*packet) + 1;
  for (unsigned i = 0; i < cnt; ++i) {
    process_set_reg(os, reg + 4 * i, packet[1 + i]);
  }
}

static void process_ib(std::ostream &os, uint32_t *curr, uint32_t const *e);
static void process_dma_ib(std::ostream &os, uint32_t *curr, uint32_t const *e);
static size_t cs_id = 0;

void process_packet3(std::ostream &os, uint32_t *packet) {
  auto op = PKT3_IT_OPCODE_G(*packet);
  auto pred = PKT3_PREDICATE(*packet);
  int i;

  /* Print the name first. */
  for (i = 0; i < ARRAY_SIZE(packet3_table); i++)
    if (packet3_table[i].op == op)
      break;
  if (i < ARRAY_SIZE(packet3_table)) {
    const char *name = sid_strings + packet3_table[i].name_offset;
    if (op == PKT3_SET_CONTEXT_REG || op == PKT3_SET_CONFIG_REG ||
        op == PKT3_SET_UCONFIG_REG || op == PKT3_SET_SH_REG || op == PKT3_SET_SH_REG_INDEX) {
      auto idx = (packet[1] >> 28) & 0x7;
      char idx_str[5] = {0};
      if (idx)
        snprintf(idx_str, 5, "(%d)", idx);
      os << COLOR_CYAN << name << idx_str << COLOR_CYAN << (pred ? "(P)" :  "") << ":\n";
    } else
      os << COLOR_GREEN << name << COLOR_CYAN << (pred ? "(P)" :  "") << ":\n";
  }
  /*else
          fprintf(f, COLOR_RED "PKT3_UNKNOWN 0x%x%s" COLOR_RESET ":\n",
                  op, predicate);*/

  switch (PKT3_IT_OPCODE_G(*packet)) {
  case PKT3_SET_CONTEXT_REG_MASK: {
    unsigned reg = packet[1] * 4 + SI_CONTEXT_REG_OFFSET;
    process_set_reg_mask(os, reg, packet[3], packet[2]);
  } break;
  case PKT3_SET_CONTEXT_REG: {
    unsigned reg = packet[1] * 4 + SI_CONTEXT_REG_OFFSET;
    for (unsigned i = 0; i < PKT_COUNT_G(packet[0]); ++i) {
      process_set_reg(os, reg + 4 * i, packet[2 + i]);
    }
  } break;
  case PKT3_LOAD_CONTEXT_REG: {
    unsigned reg = packet[3] * 4 + SI_CONTEXT_REG_OFFSET;
    print_named_value(os, "ADDR_LO", packet[1], 32);
    print_named_value(os, "ADDR_HI", packet[2] & 0xffff, 32);
    print_reg_name(os, packet[3] * 4 + SI_CONTEXT_REG_OFFSET);
    print_named_value(os, "DWORDS", packet[4], 32);
  } break;
  case PKT3_SET_SH_REG: {
    unsigned reg = packet[1] * 4 + SI_SH_REG_OFFSET;
    for (unsigned i = 0; i < PKT_COUNT_G(packet[0]); ++i) {
      process_set_reg(os, reg + 4 * i, packet[2 + i]);
    }
  } break;
  case PKT3_SET_SH_REG_INDEX: {
    unsigned reg = packet[1] * 4 + SI_SH_REG_OFFSET;
    for (unsigned i = 0; i < PKT_COUNT_G(packet[0]); ++i) {
      process_set_reg(os, reg + 4 * i, packet[2 + i]);
    }
  } break;
  case PKT3_SET_CONFIG_REG: {
    unsigned reg = packet[1] * 4 + SI_CONFIG_REG_OFFSET;
    for (unsigned i = 0; i < PKT_COUNT_G(packet[0]); ++i) {
      process_set_reg(os, reg + 4 * i, packet[2 + i]);
    }
  } break;
  case PKT3_SET_UCONFIG_REG: {
    unsigned reg = packet[1] * 4 + CIK_UCONFIG_REG_OFFSET;
    for (unsigned i = 0; i < PKT_COUNT_G(packet[0]); ++i) {
      process_set_reg(os, reg + 4 * i, packet[2 + i]);
    }
  } break;
  case PKT3_CONTEXT_CONTROL:
    print_named_value(os, "LOAD_CONTROL", packet[1], 32);
    print_named_value(os, "SHADOW_CONTROL", packet[2], 32);
    break;
  case PKT3_DRAW_PREAMBLE:
    si_dump_reg(os, R_030908_VGT_PRIMITIVE_TYPE, packet[1], ~0);
    si_dump_reg(os, R_028AA8_IA_MULTI_VGT_PARAM, packet[2], ~0);
    si_dump_reg(os, R_028B58_VGT_LS_HS_CONFIG, packet[3], ~0);
    break;
  case PKT3_ACQUIRE_MEM:
    si_dump_reg(os, R_0301F0_CP_COHER_CNTL, packet[1], ~0);
    si_dump_reg(os, R_0301F4_CP_COHER_SIZE, packet[2], ~0);
    si_dump_reg(os, R_030230_CP_COHER_SIZE_HI, packet[3], ~0);
    si_dump_reg(os, R_0301F8_CP_COHER_BASE, packet[4], ~0);
    si_dump_reg(os, R_0301E4_CP_COHER_BASE_HI, packet[5], ~0);
    print_named_value(os, "POLL_INTERVAL", packet[6], 16);
    break;
  case PKT3_SURFACE_SYNC:
    si_dump_reg(os, R_0085F0_CP_COHER_CNTL, packet[1], ~0);
    si_dump_reg(os, R_0085F4_CP_COHER_SIZE, packet[2], ~0);
    si_dump_reg(os, R_0085F8_CP_COHER_BASE, packet[3], ~0);
    print_named_value(os, "POLL_INTERVAL", packet[4], 16);
    break;
  case PKT3_EVENT_WRITE:
    si_dump_reg(os, R_028A90_VGT_EVENT_INITIATOR, packet[1],
                S_028A90_EVENT_TYPE(~0));
    print_named_value(os, "EVENT_INDEX", (packet[1] >> 8) & 0xf, 4);
    print_named_value(os, "INV_L2", (packet[1] >> 20) & 0x1, 1);
    if (PKT_COUNT_G(packet[0]) > 0) {
      print_named_value(os, "ADDRESS_LO", packet[2], 32);
      print_named_value(os, "ADDRESS_HI", packet[3], 16);
    }
    break;
  case PKT3_EVENT_WRITE_EOP:
    si_dump_reg(os, R_028A90_VGT_EVENT_INITIATOR, packet[1],
                S_028A90_EVENT_TYPE(~0));
    print_named_value(os, "EVENT_INDEX", (packet[1] >> 8) & 0xf, 3);
    print_named_value(os, "DATA_SEL", (packet[3] >> 29) & 0x7, 3);
    print_named_value(os, "ADDR_LO", packet[2], 32);
    print_named_value(os, "ADDR_HI", packet[3] & 0xffff, 32);
    print_named_value(os, "SEQ_LO", packet[4], 32);
    print_named_value(os, "SEQ_HI", packet[5], 32);
    break;
  case PKT3_RELEASE_MEM:
    si_dump_reg(os, R_028A90_VGT_EVENT_INITIATOR, packet[1],
                S_028A90_EVENT_TYPE(~0));
    print_named_value(os, "EVENT_INDEX", (packet[1] >> 8) & 0xf, 4);
    print_named_value(os, "DATA_SEL", (packet[2] >> 29) & 0x7, 3);
    print_named_value(os, "ADDR_LO", packet[3], 32);
    print_named_value(os, "ADDR_HI", packet[4], 32);
    print_named_value(os, "SEQ_LO", packet[5], 32);
    print_named_value(os, "SEQ_HI", packet[6], 32);
    break;
  case PKT3_DRAW_INDEX_AUTO:
    si_dump_reg(os, R_030930_VGT_NUM_INDICES, packet[1], ~0);
    si_dump_reg(os, R_0287F0_VGT_DRAW_INITIATOR, packet[2], ~0);
    break;
  case PKT3_DRAW_INDEX_2:
    si_dump_reg(os, R_028A78_VGT_DMA_MAX_SIZE, packet[1], ~0);
    si_dump_reg(os, R_0287E8_VGT_DMA_BASE, packet[2], ~0);
    si_dump_reg(os, R_0287E4_VGT_DMA_BASE_HI, packet[3], ~0);
    si_dump_reg(os, R_030930_VGT_NUM_INDICES, packet[4], ~0);
    si_dump_reg(os, R_0287F0_VGT_DRAW_INITIATOR, packet[5], ~0);
    break;
  case PKT3_INDEX_TYPE:
    si_dump_reg(os, R_028A7C_VGT_DMA_INDEX_TYPE, packet[1], ~0);
    break;
  case PKT3_NUM_INSTANCES:
    si_dump_reg(os, R_030934_VGT_NUM_INSTANCES, packet[1], ~0);
    break;
  case PKT3_WRITE_DATA:
    si_dump_reg(os, R_370_CONTROL, packet[1], ~0);
    si_dump_reg(os, R_371_DST_ADDR_LO, packet[2], ~0);
    si_dump_reg(os, R_372_DST_ADDR_HI, packet[3], ~0);
    if (packet[2] == config_reg) {
      packet[11] = fui(128.0);
      std::cerr << "write config_reg " << std::hex << packet[11] << " "
                << std::dec << uif(packet[11]) << "\n";
    }
    for (unsigned i = 2; i < PKT_COUNT_G(packet[0]); i++) {
      print_spaces(os, INDENT_PKT);
      os << "0x" << std::setw(8) << std::setfill('0') << std::hex
         << packet[2 + i] << std::dec << "\n";
    }
    break;
  case PKT3_CP_DMA:
    si_dump_reg(os, R_410_CP_DMA_WORD0, packet[1], ~0);
    si_dump_reg(os, R_411_CP_DMA_WORD1, packet[2], ~0);
    si_dump_reg(os, R_412_CP_DMA_WORD2, packet[3], ~0);
    si_dump_reg(os, R_413_CP_DMA_WORD3, packet[4], ~0);
    si_dump_reg(os, R_414_COMMAND, packet[5], ~0);
    break;
  case PKT3_DMA_DATA:
    si_dump_reg(os, R_500_DMA_DATA_WORD0, packet[1], ~0);
    si_dump_reg(os, R_501_SRC_ADDR_LO, packet[2], ~0);
    si_dump_reg(os, R_502_SRC_ADDR_HI, packet[3], ~0);
    si_dump_reg(os, R_503_DST_ADDR_LO, packet[4], ~0);
    si_dump_reg(os, R_504_DST_ADDR_HI, packet[5], ~0);
    si_dump_reg(os, R_414_COMMAND, packet[6], ~0);
    break;
  case PKT3_COPY_DATA:
    print_named_value(os, "SRC_SEL", (packet[1] >> 0) & 0xf, 4);
    print_named_value(os, "DST_SEL", (packet[1] >> 8) & 0xf, 4);
    print_named_value(os, "COUNT_SEL", (packet[1] >> 16) & 1, 1);
    print_named_value(os, "WR_CONFIRM", (packet[1] >> 20) & 1, 1);
    print_named_value(os, "ENGINE_SEL", (packet[1] >> 30) & 3, 2);
    print_named_value(os, "SRC_ADDR_LO", packet[2], 32);
    print_named_value(os, "SRC_ADDR_HI", packet[3], 32);
    print_named_value(os, "DST_ADDR_LO", packet[4], 32);
    print_named_value(os, "DST_ADDR_HI", packet[5], 32);
    break;
  case PKT3_INCREMENT_CE_COUNTER:
    print_named_value(os, "CE_COUNTER_DUMMY", packet[1], 32);
    for (unsigned i = 1; i <= PKT_COUNT_G(packet[0]); i++) {
      print_spaces(os, INDENT_PKT);
    }
    break;
  case PKT3_INCREMENT_DE_COUNTER:
    print_named_value(os, "DE_COUNTER_DUMMY", packet[1], 32);
    for (unsigned i = 1; i <= PKT_COUNT_G(packet[0]); i++) {
      print_spaces(os, INDENT_PKT);
    }
    break;
  case PKT3_WAIT_ON_CE_COUNTER:
    print_named_value(os, "WAIT_CE_COUNTER_DUMMY", packet[1], 32);
    for (unsigned i = 1; i <= PKT_COUNT_G(packet[0]); i++) {
      print_spaces(os, INDENT_PKT);
    }
    break;
  case PKT3_DUMP_CONST_RAM:
    print_named_value(os, "OFFSET", packet[1], 32);
    print_named_value(os, "SIZE", packet[2], 32);
    print_named_value(os, "ADDR_LO", packet[3], 32);
    print_named_value(os, "ADDR_HI", packet[4], 32);
    for (unsigned i = 4; i <= PKT_COUNT_G(packet[0]); i++) {
      print_spaces(os, INDENT_PKT);
      os << "warn 0x" << std::setw(8) << std::setfill('0') << std::hex
         << packet[1 + i] << std::dec << "\n";
    }
    break;
  case PKT3_LOAD_CONST_RAM:
    print_named_value(os, "ADDR_LO", packet[1], 32);
    print_named_value(os, "ADDR_HI", packet[2], 32);
    print_named_value(os, "SIZE", packet[3], 32);
    print_named_value(os, "OFFSET", packet[4], 32);
    for (unsigned i = 4; i <= PKT_COUNT_G(packet[0]); i++) {
      print_spaces(os, INDENT_PKT);
      os << "warn 0x" << std::setw(8) << std::setfill('0') << std::hex
         << packet[1 + i] << std::dec << "\n";
    }
    break;
  case PKT3_WRITE_CONST_RAM:
    print_named_value(os, "OFFSET", packet[1], 32);
    for (unsigned i = 0; i < PKT_COUNT_G(packet[0]); i++) {
      print_spaces(os, INDENT_PKT);
      os << "0x" << std::setw(8) << std::setfill('0') << std::hex
         << packet[2 + i] << std::dec << "\n";
    }
    break;
  case PKT3_DRAW_INDEX_INDIRECT_MULTI:
    for (unsigned i = 0; i <= PKT_COUNT_G(packet[0]); i++) {
      print_spaces(os, INDENT_PKT);
      os << "0x" << std::setw(8) << std::setfill('0') << std::hex
         << packet[1 + i] << std::dec << "\n";
    }
    break;
  case PKT3_SET_BASE:
    for (unsigned i = 0; i <= PKT_COUNT_G(packet[0]); i++) {
      print_spaces(os, INDENT_PKT);
      os << "0x" << std::setw(8) << std::setfill('0') << std::hex
         << packet[1 + i] << std::dec << "\n";
    }
    break;
  default:
  case PKT3_INDEX_BASE:
    for (unsigned i = 0; i <= PKT_COUNT_G(packet[0]); i++) {
      print_spaces(os, INDENT_PKT);
      os << "0x" << std::setw(8) << std::setfill('0') << std::hex
         << packet[1 + i] << std::dec << "\n";
    }
    break;
  case PKT3_INDIRECT_BUFFER_CIK:
  case PKT3_INDIRECT_BUFFER_CONST: {
    print_named_value(os, "IB_BASE_LO", packet[1], 32);
    print_named_value(os, "IB_BASE_HI", packet[2], 32);
    print_named_value(os, "IB_SIZE", packet[3] & 0xFFFFF, 20);
    print_named_value(os, "CHAIN", (packet[3] >> 20) & 1, 1);
    print_named_value(os, "VALID", (packet[3] >> 23) & 1, 1);
    std::uint64_t va = static_cast<std::uint64_t>(packet[2]) << 32;
    va |= packet[1];
    unsigned words = packet[3] & 0xfffff;
    uint32_t *data = (uint32_t *)get_ptr(va, words * 4);
    process_ib(os, data, data + words);
  } break;
  case PKT3_NOP:
    os << "     trace id: 0x" << std::setw(8) << std::setfill('0') << std::hex
         << packet[1] << std::dec << "\n";
  }
}

static void process_ib(std::ostream &os, uint32_t *curr, uint32_t const *e) {
  while (curr != e) {
    if (curr > e) {
      std::cerr << "went past end of IB at CS " << cs_id << ": " << std::hex << curr << " " << e
                << std::endl;
      abort();
    }
    switch (PKT_TYPE_G(*curr)) {
    case 0:
      process_packet0(os, curr);
      curr += 2 + PKT_COUNT_G(*curr);
      break;
    case 2:
      curr += 1;
      break;
    case 3:
      if (*curr == 0xffff1000u) {
        ++curr;
        break;
      }
      process_packet3(os, curr);
      curr += 2 + PKT_COUNT_G(*curr);
      break;
    default:
      os << "unknown packet type " << PKT_TYPE_G(*curr) << std::hex << " "
         << *curr << std::dec << "\n";
      abort();
    }
  }
}

static void process_dma_ib(std::ostream &os, uint32_t *curr, uint32_t const *e) {
  while (curr != e) {
    if (curr > e) {
      std::cerr << "went past end of IB at CS " << cs_id << ": " << std::hex << curr << " " << e
                << std::endl;
      abort();
    }
    uint32_t val = curr[0];
    uint32_t op = val & 0xff;
    uint32_t pkt_count;
    switch (op) {
    case CIK_SDMA_OPCODE_NOP:
      ++curr;
      os << "DMA NOP" << "\n";
      break;
    case CIK_SDMA_OPCODE_COPY: {
      uint32_t sub_op = (val >> 8) & 0xff;
      switch (sub_op) {
      case CIK_SDMA_COPY_SUB_OPCODE_LINEAR:
	pkt_count = 7;
	os << "DMA COPY LINEAR" << "\n";
	print_named_value(os, "SIZE", curr[1], 32);
	print_named_value(os, "OFFSET", curr[2], 32);
	print_named_value(os, "SRC_ADDR_LO", curr[3], 32);
	print_named_value(os, "SRC_ADDR_HI", curr[4], 32);
	print_named_value(os, "DST_ADDR_LO", curr[5], 32);
	print_named_value(os, "DST_ADDR_HI", curr[6], 32);
	break;
      case CIK_SDMA_COPY_SUB_OPCODE_TILED:
	pkt_count = 12;
	os << "DMA COPY TILED" << "\n";
	print_named_value(os, "TILED_ADDR_LO", curr[1], 32);
	print_named_value(os, "TILED_ADDR_HI", curr[2], 32);
	print_named_value(os, "DW_3", curr[3], 32);
	print_named_value(os, "SLICE_PITCH", curr[4], 32);
	print_named_value(os, "DW_5", curr[5], 32);
	print_named_value(os, "DW_6", curr[6], 32);
	print_named_value(os, "DW_7", curr[7], 32);
	print_named_value(os, "LINEAR_ADDR_LO", curr[8], 32);
	print_named_value(os, "LINEAR_ADDR_HI", curr[9], 32);
	print_named_value(os, "LINEAR_PITCH", curr[10], 32);
	print_named_value(os, "COUNT", curr[11], 32);	
	break;
      case CIK_SDMA_COPY_SUB_OPCODE_LINEAR_SUB_WINDOW:
	pkt_count = 13;
	os << "DMA COPY LINEAR SUB WINDOW 0x" << std::hex << curr[0] << std::dec << "\n";
	print_named_value(os, "SRC_ADDR_LO", curr[1], 32);
	print_named_value(os, "SRC_ADDR_HI", curr[2], 32);
	print_named_value(os, "SRC_XY", curr[3], 32);
	print_named_value(os, "SRC_PITCH", curr[4], 32);
	print_named_value(os, "SRC_SLICE_PITCH", curr[5], 32);
	print_named_value(os, "DST_ADDR_LO", curr[6], 32);
	print_named_value(os, "DST_ADDR_HI", curr[7], 32);
	print_named_value(os, "DST_XY", curr[8], 32);
	print_named_value(os, "DST_Z_PITCH", curr[9], 32);
	print_named_value(os, "DST_SLICE_PITCH", curr[10], 32);
	print_named_value(os, "W_H", curr[11], 32);
	print_named_value(os, "DEPTH", curr[12], 32);
	break;
      case CIK_SDMA_COPY_SUB_OPCODE_TILED_SUB_WINDOW:
	pkt_count = 14;
	os << "DMA COPY TILED SUB WINDOW 0x" << std::hex << curr[0] << std::dec << "\n";
	print_named_value(os, "X_ADDR_LO", curr[1], 32);
	print_named_value(os, "X_ADDR_HI", curr[2], 32);
	print_named_value(os, "X_XY", curr[3], 32);
	print_named_value(os, "X_PITCH", curr[4], 32);
	print_named_value(os, "X_SRC_SLICE_PITCH", curr[5], 32);
	print_named_value(os, "TILE_INFO", curr[6], 32);
	print_named_value(os, "Y_ADDR_LO", curr[7], 32);
	print_named_value(os, "Y_ADDR_HI", curr[8], 32);
	print_named_value(os, "Y_XY", curr[9], 32);
	print_named_value(os, "Y_Z_PITCH", curr[10], 32);
	print_named_value(os, "Y_SLICE_PITCH", curr[11], 32);
	print_named_value(os, "W_H", curr[12], 32);
	print_named_value(os, "DEPTH", curr[13], 32);
	break;
      case CIK_SDMA_COPY_SUB_OPCODE_T2T_SUB_WINDOW:
	pkt_count = 15;
	os << "DMA COPY T2T SUB WINDOW 0x" << std::hex << curr[0] << std::dec << "\n";
	print_named_value(os, "SRC_ADDR_LO", curr[1], 32);
	print_named_value(os, "SRC_ADDR_HI", curr[2], 32);
	print_named_value(os, "SRC_XY", curr[3], 32);
	print_named_value(os, "SRC_PITCH", curr[4], 32);
	print_named_value(os, "SRC_SLICE_PITCH", curr[5], 32);
	print_named_value(os, "SRC_TILE_INFO", curr[6], 32);
	print_named_value(os, "DST_ADDR_LO", curr[7], 32);
	print_named_value(os, "DST_ADDR_HI", curr[8], 32);
	print_named_value(os, "DST_XY", curr[9], 32);
	print_named_value(os, "DST_Z_PITCH", curr[10], 32);
	print_named_value(os, "DST_SLICE_PITCH", curr[11], 32);
	print_named_value(os, "DST_TILE_INFO", curr[12], 32);
	print_named_value(os, "W_H", curr[13], 32);
	print_named_value(os, "DEPTH", curr[14], 32);
	break;
      default:
	os << "DMA COPY UNKNOWN" << "\n";
	break;
      }
      curr += pkt_count;
      break;
    }
    case CIK_SDMA_OPCODE_WRITE: {
      uint32_t sub_op = (val >> 8) & 0xff;
      switch (sub_op) {
      case SDMA_WRITE_SUB_OPCODE_LINEAR:
	os << "DMA WRITE LINEAR" << "\n";
	print_named_value(os, "DST_ADDR_LO", curr[1], 32);
	print_named_value(os, "DST_ADDR_HI", curr[2], 32);
	print_named_value(os, "NUM_DWORDS", curr[3], 32);
	pkt_count = curr[3] + 4;
	for (unsigned i = 0; i < curr[3]; i++) {
	  print_spaces(os, INDENT_PKT);
	  os << "0x" << std::setw(8) << std::setfill('0') << std::hex
	     << curr[4 + i] << std::dec << "\n";
	}
	break;
      case SDMA_WRITE_SUB_OPCODE_TILED:
	os << "DMA WRITE TILED" << "\n";
	pkt_count = curr[8] + 10;
	break;
      default:
	os << "DMA WRITE UNKNOWN" << "\n";
	break;
      }
      curr += pkt_count;
      break;
    }
    case CIK_SDMA_OPCODE_INDIRECT_BUFFER:
      curr += 6;
      break;
    case CIK_SDMA_PACKET_CONSTANT_FILL:
      os << "DMA CONSTANT FILL" << "\n";
      print_named_value(os, "ADDR_LO", curr[1], 32);
      print_named_value(os, "ADDR_HI", curr[2], 32);
      print_named_value(os, "DATA", curr[3], 32);
      print_named_value(os, "FILLSIZE", curr[4], 32);
      curr += 5;
      break;
    case CIK_SDMA_PACKET_TIMESTAMP:
      os << "DMA TIMESTAMP" << "\n";
      curr += 3;
      break;
    case CIK_SDMA_PACKET_ATOMIC:
      os << "DMA ATOMIC" << "\n";
      break;
    case CIK_SDMA_PACKET_SEMAPHORE:
      os << "DMA SEM" << "\n";
      curr += 3;
      break;
    default:
      os << "DMA UNKNOWN 0x" << std::hex << curr[0] << std::dec << "\n";
      curr++;
      break;
    }
  }
}

int amdgpu_cs_submit(amdgpu_context_handle context, uint64_t flags,
                     struct amdgpu_cs_request *ibs_request,
                     uint32_t number_of_requests) {

  for (unsigned i = 0; i < number_of_requests; ++i) {
    for (unsigned j = 0; j < ibs_request[i].number_of_ibs; ++j) {
      auto addr = ibs_request[i].ibs[j].ib_mc_address;
      auto size = ibs_request[i].ibs[j].size;
      std::ofstream out0(output_dir + "cs." + std::to_string(cs_id) +
                         ".type.txt");
      out0 << ibs_request[i].ip_type << "\n";
      uint32_t *data = (uint32_t *)get_ptr(addr, size * 4);
      if (data) {
        std::string cs_type = "unknown";
        if (ibs_request[i].ip_type == AMDGPU_HW_IP_DMA)
	  cs_type = "dma";
        else if (ibs_request[i].ibs[j].flags == 0)
          cs_type = "de";
        else if (ibs_request[i].ibs[j].flags == 1)
          cs_type = "ce";
        else if (ibs_request[i].ibs[j].flags == 3)
          cs_type = "ce_preamble";

        std::ofstream out(output_dir + "cs." + std::to_string(cs_id) + "." +
                          cs_type + ".txt");
        out << std::hex << addr << std::dec << "\n";
	if (ibs_request[i].ip_type == AMDGPU_HW_IP_DMA)
	  process_dma_ib(out, data, data + size);
	else
	  process_ib(out, data, data + size);
      }
    }
    ++cs_id;
  }

  return ((int (*)(amdgpu_context_handle, uint64_t, struct amdgpu_cs_request *,
                   uint32_t))_dl_sym(RTLD_NEXT, "amdgpu_cs_submit",
                                     (void *)amdgpu_cs_submit))(
      context, flags, ibs_request, number_of_requests);
}

int amdgpu_cs_submit_raw(amdgpu_device_handle device,
			 amdgpu_context_handle context,
			 amdgpu_bo_list_handle resources,
			 int num_chunks,
			 struct drm_amdgpu_cs_chunk *chunks,
			 uint64_t *seq_no)
{
  for (unsigned i = 0; i < num_chunks; ++i) {
    struct drm_amdgpu_cs_chunk_data *chunk_data;
    if (chunks[i].chunk_id != AMDGPU_CHUNK_ID_IB)
      continue;
    chunk_data = (struct drm_amdgpu_cs_chunk_data *)(uintptr_t)chunks[i].chunk_data;
    auto addr = chunk_data->ib_data.va_start;
    auto size = chunk_data->ib_data.ib_bytes / 4;

    std::ofstream out0(output_dir + "cs." + std::to_string(cs_id) +
                       ".type.txt");
    out0 << chunk_data->ib_data.ip_type << "\n";
    uint32_t *data = (uint32_t *)get_ptr(addr, size * 4);
    if (data) {
      std::string cs_type = "unknown";
      if (chunk_data->ib_data.ip_type == AMDGPU_HW_IP_DMA)
	cs_type = "dma";
      else if (chunk_data->ib_data.flags == 0)
        cs_type = "de";
      else if (chunk_data->ib_data.flags == 1)
        cs_type = "ce";
      else if (chunk_data->ib_data.flags == 3)
        cs_type = "ce_preamble";

      std::ofstream out(output_dir + "cs." + std::to_string(cs_id) + "." +
                        cs_type + ".txt");
      out << std::hex << addr << std::dec << "\n";
      if (chunk_data->ib_data.ip_type == AMDGPU_HW_IP_DMA)
        process_dma_ib(out, data, data + size);
      else
	process_ib(out, data, data + size);
    }
    ++cs_id;
  }
  return ((int (*)(amdgpu_device_handle, amdgpu_context_handle, amdgpu_bo_list_handle, int, struct drm_amdgpu_cs_chunk *,
                   uint64_t *))_dl_sym(RTLD_NEXT, "amdgpu_cs_submit_raw",
                                     (void *)amdgpu_cs_submit_raw))(
      device, context, resources, num_chunks, chunks, seq_no);
}

int amdgpu_cs_submit_raw2(amdgpu_device_handle device,
			 amdgpu_context_handle context,
			 uint32_t bo_list_handle,
			 int num_chunks,
			 struct drm_amdgpu_cs_chunk *chunks,
			 uint64_t *seq_no)
{
  for (unsigned i = 0; i < num_chunks; ++i) {
    struct drm_amdgpu_cs_chunk_data *chunk_data;
    if (chunks[i].chunk_id != AMDGPU_CHUNK_ID_IB)
      continue;
    chunk_data = (struct drm_amdgpu_cs_chunk_data *)(uintptr_t)chunks[i].chunk_data;
    auto addr = chunk_data->ib_data.va_start;
    auto size = chunk_data->ib_data.ib_bytes / 4;

    std::ofstream out0(output_dir + "cs." + std::to_string(cs_id) +
                       ".type.txt");
    out0 << chunk_data->ib_data.ip_type << "\n";
    uint32_t *data = (uint32_t *)get_ptr(addr, size * 4);
    if (data) {
      std::string cs_type = "unknown";
      if (chunk_data->ib_data.ip_type == AMDGPU_HW_IP_DMA)
	cs_type = "dma";
      else if (chunk_data->ib_data.flags == 0)
        cs_type = "de";
      else if (chunk_data->ib_data.flags == 1)
        cs_type = "ce";
      else if (chunk_data->ib_data.flags == 3)
        cs_type = "ce_preamble";

      std::ofstream out(output_dir + "cs." + std::to_string(cs_id) + "." +
                        cs_type + ".txt");
      out << std::hex << addr << std::dec << "\n";
      if (chunk_data->ib_data.ip_type == AMDGPU_HW_IP_DMA)
        process_dma_ib(out, data, data + size);
      else
	process_ib(out, data, data + size);
    }
    ++cs_id;
  }
  return ((int (*)(amdgpu_device_handle, amdgpu_context_handle, uint32_t, int, struct drm_amdgpu_cs_chunk *,
                   uint64_t *))_dl_sym(RTLD_NEXT, "amdgpu_cs_submit_raw2",
                                     (void *)amdgpu_cs_submit_raw2))(
      device, context, bo_list_handle, num_chunks, chunks, seq_no);
}

int amdgpu_bo_alloc(amdgpu_device_handle dev,
                    struct amdgpu_bo_alloc_request *alloc_buffer,
                    amdgpu_bo_handle *buf_handle) {
  std::lock_guard<std::mutex> lock(global_mutex);

  auto ret = ((int (*)(amdgpu_device_handle, struct amdgpu_bo_alloc_request *,
                       amdgpu_bo_handle *))_dl_sym(RTLD_NEXT, "amdgpu_bo_alloc",
                                                   (void *)amdgpu_bo_alloc))(
      dev, alloc_buffer, buf_handle);
  if (ret) {
    return ret;
  }

  buffers[*buf_handle];
  return ret;
}

int amdgpu_bo_free(amdgpu_bo_handle buf_handle) {
  std::lock_guard<std::mutex> lock(global_mutex);

  auto it = buffers.find(buf_handle);
  if (it != buffers.end()) {
    if (it->second.data)
      ((void (*)(amdgpu_bo_handle))_dl_sym(RTLD_NEXT, "amdgpu_bo_cpu_unmap",
                                           (void *)amdgpu_bo_cpu_unmap))(
          buf_handle);
    buffers.erase(it);
  }

  ((int (*)(amdgpu_bo_handle))_dl_sym(RTLD_NEXT, "amdgpu_bo_free",
                                      (void *)amdgpu_bo_free))(buf_handle);
}

int amdgpu_bo_cpu_map(amdgpu_bo_handle buf_handle, void **cpu) {
  std::lock_guard<std::mutex> lock(global_mutex);
  auto it = buffers.find(buf_handle);
  if (it != buffers.end() && it->second.data) {
    *cpu = it->second.data;
    return 0;
  }

  int ret = ((int (*)(amdgpu_bo_handle, void **))_dl_sym(
      RTLD_NEXT, "amdgpu_bo_cpu_map", (void *)amdgpu_bo_cpu_map))(buf_handle,
                                                                  cpu);
  if (ret)
    return ret;

  if (it != buffers.end()) {
    it->second.data = *cpu;
  }
  return 0;
}

int amdgpu_bo_cpu_unmap(amdgpu_bo_handle buf_handle) {
  int ret = ((int (*)(amdgpu_bo_handle))_dl_sym(
      RTLD_NEXT, "amdgpu_bo_cpu_unmap", (void *)amdgpu_bo_cpu_unmap))(
      buf_handle);
  if (ret)
    return ret;

  std::lock_guard<std::mutex> lock(global_mutex);
  auto it = buffers.find(buf_handle);
  if (it != buffers.end()) {
    it->second.data = nullptr;
  }
  return 0;
}

int amdgpu_bo_va_op(amdgpu_bo_handle bo, uint64_t offset, uint64_t size,
                    uint64_t addr, uint64_t flags, uint32_t ops) {
  int ret = ((int (*)(amdgpu_bo_handle, uint64_t, uint64_t, uint64_t, uint64_t,
                      uint32_t))_dl_sym(RTLD_NEXT, "amdgpu_bo_va_op",
                                        (void *)amdgpu_bo_va_op))(
      bo, offset, size, addr, flags, ops);
  if (ret)
    return ret;

  std::lock_guard<std::mutex> lock(global_mutex);
  if (ops == AMDGPU_VA_OP_MAP) {
    Map_info info;
    info.bo = bo;
    info.addr = addr;
    info.size = size;
    info.offset = offset;
    maps[addr] = info;
  } else if (ops == AMDGPU_VA_OP_UNMAP) {
    auto it = maps.find(addr);
    if (it != maps.end()) {
      maps.erase(it);
    }
  }
  return ret;
}

int amdgpu_bo_va_op_raw(amdgpu_device_handle dev, amdgpu_bo_handle bo, uint64_t offset, uint64_t size,
                    uint64_t addr, uint64_t flags, uint32_t ops) {
  int ret = ((int (*)(amdgpu_device_handle, amdgpu_bo_handle, uint64_t, uint64_t, uint64_t, uint64_t,
                      uint32_t))_dl_sym(RTLD_NEXT, "amdgpu_bo_va_op_raw",
                                        (void *)amdgpu_bo_va_op_raw))(
      dev, bo, offset, size, addr, flags, ops);
  if (ret)
    return ret;

  std::lock_guard<std::mutex> lock(global_mutex);
  if (ops == AMDGPU_VA_OP_MAP) {
    Map_info info;
    info.bo = bo;
    info.addr = addr;
    info.size = size;
    info.offset = offset;
    maps[addr] = info;
  } else if (ops == AMDGPU_VA_OP_UNMAP) {
    auto it = maps.find(addr);
    if (it != maps.end()) {
      maps.erase(it);
    }
  }
  return ret;
}

extern "C" int amdgpu_bo_va_op_refcounted(amdgpu_device_handle dev,
amdgpu_bo_handle bo, uint64_t offset, uint64_t size,
                    uint64_t addr, uint64_t flags, uint32_t ops) {
  int ret = ((int (*)(amdgpu_device_handle, amdgpu_bo_handle, uint64_t, uint64_t, uint64_t, uint64_t,
                      uint32_t))_dl_sym(RTLD_NEXT, "amdgpu_bo_va_op_refcounted",
                                        (void *)amdgpu_bo_va_op))(
      dev, bo, offset, size, addr, flags, ops);
  if (ret)
    return ret;

  std::lock_guard<std::mutex> lock(global_mutex);
  if (ops == AMDGPU_VA_OP_MAP) {
    Map_info info;
    info.bo = bo;
    info.addr = addr;
    info.size = size;
    info.offset = offset;
    maps[addr] = info;
  } else if (ops == AMDGPU_VA_OP_UNMAP) {
    auto it = maps.find(addr);
    if (it != maps.end()) {
      maps.erase(it);
    }
  }
  return ret;
}

extern "C" void *dlsym(void *handle, const char *name) {
  if (!strcmp(name, "dlsym"))
    return (void *)dlsym;
  if (!strcmp(name, "amdgpu_cs_submit_raw2"))
    return (void *)amdgpu_cs_submit_raw2;
  if (!strcmp(name, "amdgpu_cs_submit_raw"))
    return (void *)amdgpu_cs_submit_raw;
  if (!strcmp(name, "amdgpu_cs_submit"))
    return (void *)amdgpu_cs_submit;
  if (!strcmp(name, "amdgpu_bo_alloc"))
    return (void *)amdgpu_bo_alloc;
  if (!strcmp(name, "amdgpu_bo_free"))
    return (void *)amdgpu_bo_free;
  if (!strcmp(name, "amdgpu_bo_cpu_map"))
    return (void *)amdgpu_bo_cpu_map;
  if (!strcmp(name, "amdgpu_bo_cpu_unmap"))
    return (void *)amdgpu_bo_cpu_unmap;
  if (!strcmp(name, "amdgpu_bo_va_op"))
    return (void *)amdgpu_bo_va_op;
  if (!strcmp(name, "amdgpu_bo_va_op_raw"))
    return (void *)amdgpu_bo_va_op_raw;
  if (!strcmp(name, "amdgpu_bo_va_op_refcounted"))
    return (void *)amdgpu_bo_va_op_refcounted;
  return get_real_dlsym()(handle, name);
}

extern "C" void *__libc_dlsym(void *, const char *);
static void *(*get_real_dlsym())(void *, const char *) {
  if (real_dlsym == NULL) {
    real_dlsym = (void *(*)(void *, const char *))_dl_sym(RTLD_NEXT, "dlsym",
                                                          (void *)dlsym);
    auto dir = getenv("INTERCEPT_DIR");
    if (!dir || dir[0] == 0)
      output_dir = "/tmp/";
    else {
      output_dir = dir;
      if (output_dir.back() != '/')
        output_dir += '/';
    }
  }

  return real_dlsym;
}
