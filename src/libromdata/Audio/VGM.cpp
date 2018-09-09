/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * VGM.hpp: VGM audio reader.                                              *
 *                                                                         *
 * Copyright (c) 2018 by David Korth.                                      *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published by the   *
 * Free Software Foundation; either version 2 of the License, or (at your  *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License       *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.   *
 ***************************************************************************/

#include "VGM.hpp"
#include "librpbase/RomData_p.hpp"

#include "vgm_structs.h"

// librpbase
#include "librpbase/common.h"
#include "librpbase/byteswap.h"
#include "librpbase/TextFuncs.hpp"
#include "librpbase/file/IRpFile.hpp"
#include "libi18n/i18n.h"
using namespace LibRpBase;

// C includes. (C++ namespace)
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstring>

// C++ includes.
#include <memory>
#include <string>
#include <vector>
using std::string;
using std::unique_ptr;
using std::vector;

namespace LibRomData {

ROMDATA_IMPL(VGM)

class VGMPrivate : public RomDataPrivate
{
	public:
		VGMPrivate(VGM *q, IRpFile *file);

	private:
		typedef RomDataPrivate super;
		RP_DISABLE_COPY(VGMPrivate)

	public:
		// VGM header.
		// NOTE: **NOT** byteswapped in memory.
		VGM_Header vgmHeader;

		/**
		 * Format an IC clock rate in Hz, kHz, MHz, or GHz.
		 * @param clock_rate Clock rate.
		 * @return IC clock rate, in Hz, kHz, MHz, or GHz. (Three decimal places.)
		 */
		string formatClockRate(unsigned int clock_rate);

		/**
		 * Load GD3 tags.
		 * @param addr Starting address of the GD3 tag block.
		 * @return Vector of tags, or empty vector on error.
		 */
		vector<string> loadGD3(unsigned int addr);
};

/** VGMPrivate **/

VGMPrivate::VGMPrivate(VGM *q, IRpFile *file)
	: super(q, file)
{
	// Clear the VGM header struct.
	memset(&vgmHeader, 0, sizeof(vgmHeader));
}

/**
 * Format an IC clock rate in Hz, kHz, MHz, or GHz.
 * @param clock_rate Clock rate.
 * @return IC clock rate, in Hz, kHz, MHz, or GHz. (Three decimal places.)
 */
string VGMPrivate::formatClockRate(unsigned int clock_rate)
{
	// TODO: Rounding?

	if (clock_rate < 1000) {
		// Hz
		return rp_sprintf(C_("VGM", "%u Hz"), clock_rate);
	} else if (clock_rate < 1000000) {
		// kHz
		const unsigned int whole = clock_rate / 1000;
		const unsigned int frac = clock_rate % 1000;
		return rp_sprintf_p(C_("VGM", "%1$u.%2$03u kHz"), whole, frac);
	} else if (clock_rate < 1000000000) {
		// MHz
		const unsigned int whole = clock_rate / 1000000;
		const unsigned int frac = (clock_rate / 1000) % 1000;
		return rp_sprintf_p(C_("VGM", "%1$u.%2$03u MHz"), whole, frac);
	} else {
		// GHz
		const unsigned int whole = clock_rate / 1000000000;
		const unsigned int frac = (clock_rate / 1000000) % 1000;
		return rp_sprintf_p(C_("VGM", "%1$u.%2$03u GHz"), whole, frac);
	}
}

/**
 * Load GD3 tags.
 * @param addr Starting address of the GD3 tag block.
 * @return Vector of tags, or empty vector on error.
 */
vector<string> VGMPrivate::loadGD3(unsigned int addr)
{
	vector<string> v_gd3;

	assert(file != nullptr);
	assert(file->isOpen());
	if (!file || !file->isOpen()) {
		return v_gd3;
	}

	GD3_Header gd3Header;
	size_t size = file->seekAndRead(addr, &gd3Header, sizeof(gd3Header));
	if (size != sizeof(gd3Header)) {
		// Seek and/or read error.
		return v_gd3;
	}

	// Validate the header.
	if (gd3Header.magic != cpu_to_le32(GD3_MAGIC) ||
	    le32_to_cpu(gd3Header.version) < 0x0100)
	{
		// Incorrect header.
		// TODO: Require exactly v1.00?
		return v_gd3;
	}

	// Length limitations:
	// - Must be an even number. (UTF-16)
	// - Minimum of 11*2 bytes; Maximum of 16 KB.
	const unsigned int length = le32_to_cpu(gd3Header.length);
	if (length % 2 != 0 || length < 11*2 || length > 16*1024) {
		// Incorrect length value.
		return v_gd3;
	}

	// Read the GD3 data.
	const unsigned int length16 = length / 2;
	unique_ptr<char16_t[]> gd3(new char16_t[length16]);
	size = file->read(gd3.get(), length);
	if (size != length) {
		// Read error.
		return v_gd3;
	}

	// Make sure the end of the GD3 data is NULL-terminated.
	if (gd3[length16-1] != 0) {
		// Not NULL-terminated.
		return v_gd3;
	}

	// Convert from NULL-terminated strings to a vector.
	// TODO: Optimize on systems where wchar_t functions are 16-bit?
	const char16_t *start = gd3.get();
	const char16_t *const endptr = start + length16;
	for (const char16_t *p = start; p < endptr; p++) {
		// Check for a NULL.
		if (*p == 0) {
			// Found a NULL!
			v_gd3.push_back(utf16le_to_utf8(start, (int)(p-start)));
			// Next string.
			start = p + 1;
		}
	}

	// TODO: Verify that it's 11 strings?
	return v_gd3;
}

/** VGM **/

/**
 * Read an VGM audio file.
 *
 * A ROM image must be opened by the caller. The file handle
 * will be dup()'d and must be kept open in order to load
 * data from the ROM image.
 *
 * To close the file, either delete this object or call close().
 *
 * NOTE: Check isValid() to determine if this is a valid ROM.
 *
 * @param file Open ROM image.
 */
VGM::VGM(IRpFile *file)
	: super(new VGMPrivate(this, file))
{
	RP_D(VGM);
	d->className = "VGM";
	d->fileType = FTYPE_AUDIO_FILE;

	if (!d->file) {
		// Could not dup() the file handle.
		return;
	}

	// Read the VGM header.
	d->file->rewind();
	size_t size = d->file->read(&d->vgmHeader, sizeof(d->vgmHeader));
	if (size != sizeof(d->vgmHeader)) {
		delete d->file;
		d->file = nullptr;
		return;
	}

	// Check if this file is supported.
	DetectInfo info;
	info.header.addr = 0;
	info.header.size = sizeof(d->vgmHeader);
	info.header.pData = reinterpret_cast<const uint8_t*>(&d->vgmHeader);
	info.ext = nullptr;	// Not needed for VGM.
	info.szFile = 0;	// Not needed for VGM.
	d->isValid = (isRomSupported_static(&info) >= 0);

	if (!d->isValid) {
		delete d->file;
		d->file = nullptr;
		return;
	}
}

/**
 * Is a ROM image supported by this class?
 * @param info DetectInfo containing ROM detection information.
 * @return Class-specific system ID (>= 0) if supported; -1 if not.
 */
int VGM::isRomSupported_static(const DetectInfo *info)
{
	assert(info != nullptr);
	assert(info->header.pData != nullptr);
	assert(info->header.addr == 0);
	if (!info || !info->header.pData ||
	    info->header.addr != 0 ||
	    info->header.size < sizeof(VGM_Header))
	{
		// Either no detection information was specified,
		// or the header is too small.
		return -1;
	}

	const VGM_Header *const vgmHeader =
		reinterpret_cast<const VGM_Header*>(info->header.pData);

	// Check the VGM magic number.
	if (vgmHeader->magic == cpu_to_be32(VGM_MAGIC)) {
		// Found the VGM magic number.
		return 0;
	}

	// Not supported.
	return -1;
}

/**
 * Get the name of the system the loaded ROM is designed for.
 * @param type System name type. (See the SystemName enum.)
 * @return System name, or nullptr if type is invalid.
 */
const char *VGM::systemName(unsigned int type) const
{
	RP_D(const VGM);
	if (!d->isValid || !isSystemNameTypeValid(type))
		return nullptr;

	// VGM has the same name worldwide, so we can
	// ignore the region selection.
	static_assert(SYSNAME_TYPE_MASK == 3,
		"VGM::systemName() array index optimization needs to be updated.");

	static const char *const sysNames[4] = {
		"Video Game Music",
		"VGM",
		"VGM",
		nullptr
	};

	return sysNames[type & SYSNAME_TYPE_MASK];
}

/**
 * Get a list of all supported file extensions.
 * This is to be used for file type registration;
 * subclasses don't explicitly check the extension.
 *
 * NOTE: The extensions include the leading dot,
 * e.g. ".bin" instead of "bin".
 *
 * NOTE 2: The array and the strings in the array should
 * *not* be freed by the caller.
 *
 * @return NULL-terminated array of all supported file extensions, or nullptr on error.
 */
const char *const *VGM::supportedFileExtensions_static(void)
{
	static const char *const exts[] = {
		".vgm",
		".vgz",	// TODO: Automatic gzip detection.
		//".vgm.gz",	// NOTE: Windows doesn't support this.

		nullptr
	};
	return exts;
}

/**
 * Get a list of all supported MIME types.
 * This is to be used for metadata extractors that
 * must indicate which MIME types they support.
 *
 * NOTE: The array and the strings in the array should
 * *not* be freed by the caller.
 *
 * @return NULL-terminated array of all supported file extensions, or nullptr on error.
 */
const char *const *VGM::supportedMimeTypes_static(void)
{
	static const char *const mimeTypes[] = {
		// Unofficial MIME types.
		"audio/x-vgm",

		nullptr
	};
	return mimeTypes;
}

/**
 * Load field data.
 * Called by RomData::fields() if the field data hasn't been loaded yet.
 * @return Number of fields read on success; negative POSIX error code on error.
 */
int VGM::loadFieldData(void)
{
	RP_D(VGM);
	if (!d->fields->empty()) {
		// Field data *has* been loaded...
		return 0;
	} else if (!d->file) {
		// File isn't open.
		return -EBADF;
	} else if (!d->isValid) {
		// Unknown file type.
		return -EIO;
	}

	// VGM header.
	const VGM_Header *const vgmHeader = &d->vgmHeader;
	// NOTE: It's very unlikely that a single VGM will have
	// all supported sound chips, so we'll reserve enough
	// fields for the base data and up to 8 sound chips,
	// assuming 2 fields per chip.
	d->fields->reserve(11+(8*2));

	// Version number. (BCD)
	unsigned int vgm_version = le32_to_cpu(vgmHeader->version);
	d->fields->addField_string(C_("VGM", "VGM Version"),
		rp_sprintf_p(C_("VGM", "%1$x.%2$02x"), vgm_version >> 8, vgm_version & 0xFF));

	// VGM data offset.
	// Header fields must end before this offset.
	unsigned int data_offset;
	if (vgm_version < 0x0150) {
		// VGM older than v1.50: Fixed start offset of 0x40.
		data_offset = 0x40;
	} else {
		data_offset = le32_to_cpu(d->vgmHeader.data_offset) + offsetof(VGM_Header, data_offset);
	}

	// NOTE: Not byteswapping when checking for 0 because
	// 0 in big-endian is the same as 0 in little-endian.

	// GD3 tags.
	if (d->vgmHeader.gd3_offset != 0) {
		// TODO: Make sure the GD3 offset is stored after the header.
		const unsigned int addr = le32_to_cpu(d->vgmHeader.gd3_offset) + offsetof(VGM_Header, gd3_offset);
		vector<string> v_gd3 = d->loadGD3(addr);

		if (!v_gd3.empty()) {
			// TODO: Option to show Japanese instead of English.
			// TODO: Optimize line count checking?
			const size_t line_count = v_gd3.size();
			if (line_count >= 1 && !v_gd3[0].empty()) {
				d->fields->addField_string(C_("VGM", "Track Name"), v_gd3[0]);
			}
			if (line_count >= 3 && !v_gd3[2].empty()) {
				d->fields->addField_string(C_("VGM", "Game Name"), v_gd3[2]);
			}
			if (line_count >= 5 && !v_gd3[4].empty()) {
				d->fields->addField_string(C_("VGM", "System Name"), v_gd3[4]);
			}
			if (line_count >= 7 && !v_gd3[6].empty()) {
				// TODO: Multiple composer handling.
				d->fields->addField_string(C_("VGM", "Composer"), v_gd3[6]);
			}
			if (line_count >= 9 && !v_gd3[8].empty()) {
				d->fields->addField_string(C_("VGM", "Release Date"), v_gd3[8]);
			}
			if (line_count >= 10 && !v_gd3[9].empty()) {
				d->fields->addField_string(C_("VGM", "VGM Ripper"), v_gd3[9]);
			}
			if (line_count >= 11 && !v_gd3[10].empty()) {
				d->fields->addField_string(C_("VGM", "Notes"), v_gd3[10]);
			}
		}
	}

	// Duration [1.00]
	d->fields->addField_string(C_("VGM", "Duration"),
		formatSampleAsTime(le32_to_cpu(vgmHeader->sample_count), VGM_SAMPLE_RATE));

	// Loop point [1.00]
	if (vgmHeader->loop_offset != 0) {
		d->fields->addField_string(C_("VGM", "Loop Offset"),
			formatSampleAsTime(le32_to_cpu(vgmHeader->loop_offset), VGM_SAMPLE_RATE));
	}

	// Framerate. [1.01]
	if (vgm_version >= 0x0101) {
		if (vgmHeader->frame_rate != 0) {
			d->fields->addField_string_numeric(C_("VGM", "Frame Rate"),
				le32_to_cpu(vgmHeader->frame_rate));
		}
	}

	// TODO:
	// - VGM 1.51: Loop modifier
	// - VGM 1.60: Volume modifier, loop base

	// SN76489 [1.00]
	const uint32_t sn76489_clk = le32_to_cpu(vgmHeader->sn76489_clk);
	if ((sn76489_clk & ~PSG_T6W28) != 0) {
		// TODO: Handle the dual-chip bit.

		// Check for T6W28.
		const char *chip_name;
		if ((sn76489_clk & PSG_T6W28) == PSG_T6W28) {
			chip_name = "T6W28";
		} else {
			chip_name = "SN76489";
		}

		d->fields->addField_string(
			rp_sprintf(C_("VGM", "%s Clock Rate"), chip_name).c_str(),
			d->formatClockRate(sn76489_clk & ~PSG_T6W28));

		// LFSR data. [1.10; defaults used for older versions]
		uint16_t lfsr_feedback = 0x0009;
		uint8_t lfsr_width = 16;
		if (vgm_version >= 0x0110) {
			if (vgmHeader->sn76489_lfsr != 0) {
				lfsr_feedback = le16_to_cpu(vgmHeader->sn76489_lfsr);
			}
			if (vgmHeader->sn76489_width != 0) {
				lfsr_width = vgmHeader->sn76489_width;
			}
		}

		d->fields->addField_string_numeric(
			rp_sprintf(C_("VGM", "%s LFSR pattern"), chip_name).c_str(),
			lfsr_feedback, RomFields::FB_HEX, 4, RomFields::STRF_MONOSPACE);
		d->fields->addField_string_numeric(
			rp_sprintf(C_("VGM", "%s LFSR width"), chip_name).c_str(),
			lfsr_width);

		// Flags. [1.51]
		uint32_t psg_flags;
		if (vgm_version >= 0x0151) {
			// NOTE: Bits 2 and 3 are active low, so invert them here.
			psg_flags = vgmHeader->sn76489_flags ^ 0x0C;
		} else {
			// No PSG flags.
			psg_flags = 0;
		}
		static const char *const psg_flags_bitfield_names[] = {
			NOP_C_("VGM|PSGFlags", "Freq 0 is 0x400"),
			NOP_C_("VGM|PSGFlags", "Output Negate"),
			NOP_C_("VGM|PSGFlags", "Stereo"),
			NOP_C_("VGM|PSGFlags", "/8 Clock Divider"),
		};
		vector<string> *const v_psg_flags_bitfield_names = RomFields::strArrayToVector_i18n(
			"VGM|PSGFlags", psg_flags_bitfield_names, ARRAY_SIZE(psg_flags_bitfield_names));
		d->fields->addField_bitfield(rp_sprintf(C_("VGM", "%s Flags"), chip_name).c_str(),
			v_psg_flags_bitfield_names, 2, psg_flags);
	}

	// Macro for sound chips that don't have any special bitflags or parameters.
	// TODO: Dual-chip support. (Ignored for now.)
#define SOUND_CHIP(field, display) \
	do { \
		if (offsetof(VGM_Header, field##_clk) < data_offset) { \
			unsigned int clk = le32_to_cpu(vgmHeader->field##_clk); \
			clk &= ~(VGM_CLK_FLAG_ALTMODE | VGM_CLK_FLAG_DUALCHIP); \
			if (clk != 0) { \
				d->fields->addField_string( \
					rp_sprintf(C_("VGM", "%s Clock Rate"), display).c_str(), \
					d->formatClockRate(le32_to_cpu(vgmHeader->field##_clk))); \
			} \
		} \
	} while (0)

	// YM2413 [1.00]
	SOUND_CHIP(ym2413, "YM2413");

	if (vgm_version >= 0x0110) {
		// YM2612 [1.10]
		SOUND_CHIP(ym2612, "YM2612");

		// YM2151 [1.10]
		SOUND_CHIP(ym2151, "YM2151");
	}

	// TODO: Optimize data offset checks.
	// If e.g. Sega PCM is out of range, the rest of the chips will also
	// be out of range, so we should skip them.
	if (vgm_version >= 0x0151) {
		// Sega PCM [1.51]
		if (offsetof(VGM_Header, sega_pcm_if_reg) < data_offset) {
			unsigned int clk = le32_to_cpu(vgmHeader->sega_pcm_clk);
			clk &= ~(VGM_CLK_FLAG_ALTMODE | VGM_CLK_FLAG_DUALCHIP);
			if (clk != 0) {
				d->fields->addField_string(
					rp_sprintf(C_("VGM", "%s Clock Rate"), "Sega PCM").c_str(),
					d->formatClockRate(clk));
				d->fields->addField_string_numeric(
					rp_sprintf(C_("VGM", "%s IF reg"), "Sega PCM").c_str(),
					le32_to_cpu(vgmHeader->sega_pcm_if_reg),
					RomFields::FB_HEX, 8, RomFields::STRF_MONOSPACE);
			}
		}

		// RF5C68 [1.51]
		SOUND_CHIP(rf5c68, "RF5C68");

		// AY8910 flags.
		// Used for YM2203, YM2608, and AY8910.
		static const char *const ay8910_flags_bitfield_names[] = {
			NOP_C_("VGM|AY8910Flags", "Legacy Output"),
			NOP_C_("VGM|AY8910Flags", "Single Output"),
			NOP_C_("VGM|AY8910Flags", "Discrete Output"),
			NOP_C_("VGM|AY8910Flags", "Raw Output"),
		};

		// YM2203 [1.51]
		if (offsetof(VGM_Header, ym2203_ay8910_flags) < data_offset) {
			unsigned int clk = le32_to_cpu(vgmHeader->ym2203_clk);
			clk &= ~(VGM_CLK_FLAG_ALTMODE | VGM_CLK_FLAG_DUALCHIP);
			if (clk != 0) {
				d->fields->addField_string(
					rp_sprintf(C_("VGM", "%s Clock Rate"), "YM2203").c_str(),
					d->formatClockRate(clk));

				// TODO: Is AY8910 type needed?
				vector<string> *const v_ay8910_flags_bitfield_names = RomFields::strArrayToVector_i18n(
					"VGM|AY8910Flags", ay8910_flags_bitfield_names, ARRAY_SIZE(ay8910_flags_bitfield_names));
				d->fields->addField_bitfield(rp_sprintf(C_("VGM", "%s Flags"), "YM2203 (AY8910)").c_str(),
					v_ay8910_flags_bitfield_names, 2, vgmHeader->ym2203_ay8910_flags);
			}
		}

		// YM2608 [1.51]
		if (offsetof(VGM_Header, ym2608_ay8910_flags) < data_offset) {
			unsigned int clk = le32_to_cpu(vgmHeader->ym2608_clk);
			clk &= ~(VGM_CLK_FLAG_ALTMODE | VGM_CLK_FLAG_DUALCHIP);
			if (clk != 0) {
				d->fields->addField_string(
					rp_sprintf(C_("VGM", "%s Clock Rate"), "YM2608").c_str(),
					d->formatClockRate(clk));

				// TODO: Is AY8910 type needed?
				vector<string> *const v_ay8910_flags_bitfield_names = RomFields::strArrayToVector_i18n(
					"VGM|AY8910Flags", ay8910_flags_bitfield_names, ARRAY_SIZE(ay8910_flags_bitfield_names));
				d->fields->addField_bitfield(rp_sprintf(C_("VGM", "%s Flags"), "YM2608 (AY8910)").c_str(),
					v_ay8910_flags_bitfield_names, 2, vgmHeader->ym2608_ay8910_flags);
			}
		}

		// YM2610/YM2610B [1.51]
		if (offsetof(VGM_Header, ym2610_clk) < data_offset) {
			unsigned int clk = le32_to_cpu(vgmHeader->ym2608_clk);
			clk &= ~VGM_CLK_FLAG_DUALCHIP;
			if ((clk & ~VGM_CLK_FLAG_ALTMODE) != 0) {
				const char *const chip_name = (clk & VGM_CLK_FLAG_ALTMODE) ? "YM2610B" : "YM2610";

				d->fields->addField_string(
					rp_sprintf(C_("VGM", "%s Clock Rate"), chip_name).c_str(),
					d->formatClockRate(clk & ~VGM_CLK_FLAG_ALTMODE));
			}
		}

		// YM2813 [1.51]
		SOUND_CHIP(ym3812, "YM3812");

		// YM3526 [1.51]
		SOUND_CHIP(ym3526, "YM3526");

		// Y8950 [1.51]
		SOUND_CHIP(y8950, "Y8950");

		// YMF262 [1.51]
		SOUND_CHIP(ymf262, "YMF262");

		// YMF278B [1.51]
		SOUND_CHIP(ymf278b, "YMF278B");

		// YMF271 [1.51]
		SOUND_CHIP(ymf271, "YMF271");

		// YMZ280B [1.51]
		SOUND_CHIP(ymz280b, "YMZ280B");

		// RF5C164 [1.51]
		SOUND_CHIP(rf5c164, "RF5C164");

		// PWM [1.51]
		SOUND_CHIP(pwm, "PWM");

		// AY8910 [1.51]
		if (offsetof(VGM_Header, ay8910_flags) < data_offset) {
			unsigned int clk = le32_to_cpu(vgmHeader->ay8910_clk);
			clk &= ~(VGM_CLK_FLAG_ALTMODE | VGM_CLK_FLAG_DUALCHIP);
			if (clk != 0) {
				const char *chip_name;
				switch (vgmHeader->ay8910_type) {
					default:
						// TODO: Print the type ID?
						chip_name = "AYxxxx";
						break;
					case 0x00:
						chip_name = "AY8910";
						break;
					case 0x01:
						chip_name = "AY8912";
						break;
					case 0x02:
						chip_name = "AY8913";
						break;
					case 0x03:
						chip_name = "AY8930";
						break;
					case 0x10:
						chip_name = "YM2149";
						break;
					case 0x11:
						chip_name = "YM3439";
						break;
					case 0x12:
						chip_name = "YMZ284";
						break;
					case 0x13:
						chip_name = "YMZ294";
						break;
				}

				d->fields->addField_string(
					rp_sprintf(C_("VGM", "%s Clock Rate"), chip_name).c_str(),
					d->formatClockRate(clk));

				vector<string> *const v_ay8910_flags_bitfield_names = RomFields::strArrayToVector_i18n(
					"VGM|AY8910Flags", ay8910_flags_bitfield_names, ARRAY_SIZE(ay8910_flags_bitfield_names));
				d->fields->addField_bitfield(rp_sprintf(C_("VGM", "%s Flags"), chip_name).c_str(),
					v_ay8910_flags_bitfield_names, 2, vgmHeader->ay8910_flags);
			}
		}
	}

	if (vgm_version >= 0x0161) {
		// Game Boy (LR35902) [1.61]
		SOUND_CHIP(dmg, "DMG");

		// NES APU (2A03) [1.61]
		if (offsetof(VGM_Header, nes_apu_clk) < data_offset) {
			unsigned int clk = le32_to_cpu(vgmHeader->nes_apu_clk);
			clk &= ~VGM_CLK_FLAG_DUALCHIP;
			if ((clk & ~VGM_CLK_FLAG_ALTMODE) != 0) {
				d->fields->addField_string(
					rp_sprintf(C_("VGM", "%s Clock Rate"), "NES APU").c_str(),
						d->formatClockRate(clk & ~VGM_CLK_FLAG_ALTMODE));

				// Bit 31 indicates presence of FDS audio hardware.
				const char *const nes_exp = (clk & VGM_CLK_FLAG_ALTMODE)
					? C_("VGM|NESExpansion", "Famicom Disk System")
					: C_("VGM|NESExpansion", "(none)");
				d->fields->addField_string(
					rp_sprintf(C_("VGM", "%s Expansions"), "NES APU").c_str(), nes_exp);
			}
		}

		// MultiPCM [1.61]
		SOUND_CHIP(multipcm, "MultiPCM");

		// uPD7759 [1.61]
		SOUND_CHIP(upd7759, "uPD7759");

		// NOTE: Ordering is done by the clock rate field,
		// not the flags field.

		// OKIM6258 [1.61]
		// TODO: Flags
		SOUND_CHIP(okim6258, "OKIM6258");

		// OKIM6295 [1.61]
		SOUND_CHIP(okim6295, "OKIM6295");

		// K051649 [1.61]
		SOUND_CHIP(k051649, "K051649");

		// K054539 [1.61]
		// TODO: Flags
		SOUND_CHIP(k054539, "K054539");

		// HuC6280 [1.61]
		SOUND_CHIP(huc6280, "HuC6280");

		// C140 [1.61]
		// TODO: Flags
		SOUND_CHIP(c140, "C140");

		// K053260 [1.61]
		SOUND_CHIP(k053260, "K053260");

		// Pokey [1.61]
		SOUND_CHIP(pokey, "Pokey");

		// QSound
		SOUND_CHIP(qsound, "QSound");
	}

	if (vgm_version >= 0x0171) {
		// SCSP [1.71]
		SOUND_CHIP(scsp, "SCSP");

		// WonderSwan [1.71]
		SOUND_CHIP(ws, "WonderSwam");

		// VSU [1.71]
		SOUND_CHIP(vsu, "VSU");

		// SAA1099 [1.71]
		SOUND_CHIP(saa1099, "SAA1099");

		// ES5503 [1.71]
		if (offsetof(VGM_Header, es5503_num_ch) < data_offset) {
			unsigned int clk = le32_to_cpu(vgmHeader->es5503_clk);
			clk &= ~(VGM_CLK_FLAG_ALTMODE | VGM_CLK_FLAG_DUALCHIP);
			if (clk != 0) {
				d->fields->addField_string(
					rp_sprintf(C_("VGM", "%s Clock Rate"), "ES5503").c_str(),
						d->formatClockRate(clk));
				d->fields->addField_string_numeric(
					rp_sprintf(C_("VGM", "%s # of Channels"), "ES5503").c_str(),
						vgmHeader->es5503_num_ch);
			}
		}

		// ES5505/ES5506 [1.71]
		if (offsetof(VGM_Header, es5505_num_ch) < data_offset) {
			unsigned int clk = le32_to_cpu(vgmHeader->es5505_clk);
			clk &= ~VGM_CLK_FLAG_DUALCHIP;
			if ((clk & ~VGM_CLK_FLAG_ALTMODE) != 0) {
				const char *const chip_name = (clk & VGM_CLK_FLAG_ALTMODE)
					? "ES5506"
					: "ES5505";

				d->fields->addField_string(
					rp_sprintf(C_("VGM", "%s Clock Rate"), chip_name).c_str(),
						d->formatClockRate(clk & ~VGM_CLK_FLAG_ALTMODE));

				d->fields->addField_string_numeric(
					rp_sprintf(C_("VGM", "%s # of Channels"), chip_name).c_str(),
						vgmHeader->es5505_num_ch);
			}
		}

		// X1-010 [1.71]
		SOUND_CHIP(x1_010, "X1-010");

		// C352 [1.71]
		if (offsetof(VGM_Header, es5505_num_ch) < data_offset) {
			unsigned int clk = le32_to_cpu(vgmHeader->c352_clk);
			clk &= ~(VGM_CLK_FLAG_ALTMODE | VGM_CLK_FLAG_DUALCHIP);
			if (clk != 0) {
				d->fields->addField_string(
					rp_sprintf(C_("VGM", "%s Clock Rate"), "C352").c_str(),
						d->formatClockRate(clk));

				d->fields->addField_string_numeric(
					rp_sprintf(C_("VGM", "%s Clock Divider"), "C352").c_str(),
						vgmHeader->c352_clk_div * 4);
			}
		}

		// GA20 [1.71]
		SOUND_CHIP(ga20, "GA20");
	}

	// Finished reading the field data.
	return static_cast<int>(d->fields->count());
}

/**
 * Load metadata properties.
 * Called by RomData::metaData() if the field data hasn't been loaded yet.
 * @return Number of metadata properties read on success; negative POSIX error code on error.
 */
int VGM::loadMetaData(void)
{
	RP_D(VGM);
	if (d->metaData != nullptr) {
		// Metadata *has* been loaded...
		return 0;
	} else if (!d->file) {
		// File isn't open.
		return -EBADF;
	} else if (!d->isValid) {
		// Unknown file type.
		return -EIO;
	}

	// Create the metadata object.
	d->metaData = new RomMetaData();

	// VGM header.
	const VGM_Header *const vgmHeader = &d->vgmHeader;
	d->metaData->reserve(6);	// Maximum of 6 metadata properties.

	// Length, in milliseconds. (non-looping)
	d->metaData->addMetaData_integer(Property::Duration,
		convSampleToMs(le32_to_cpu(vgmHeader->sample_count), VGM_SAMPLE_RATE));

	// Attempt to load the GD3 tags.
	if (d->vgmHeader.gd3_offset != 0) {
		// TODO: Make sure the GD3 offset is stored after the header.
		const unsigned int addr = le32_to_cpu(d->vgmHeader.gd3_offset) + offsetof(VGM_Header, gd3_offset);
		vector<string> v_gd3 = d->loadGD3(addr);
		if (!v_gd3.empty()) {
			// TODO: Option to show Japanese instead of English.
			// TODO: Optimize line count checking?
			const size_t line_count = v_gd3.size();
			if (line_count >= 1 && !v_gd3[0].empty()) {
				d->metaData->addMetaData_string(Property::Title, v_gd3[0]);
			}
			if (line_count >= 3 && !v_gd3[2].empty()) {
				// NOTE: Not exactly "album"...
				d->metaData->addMetaData_string(Property::Album, v_gd3[2]);
			}
			/*if (line_count >= 5 && !v_gd3[4].empty()) {
				// FIXME: No property for this...
				d->metaData->addMetaData_string(Property::SystemName, v_gd3[4]);
			}*/
			if (line_count >= 7 && !v_gd3[6].empty()) {
				// TODO: Multiple composer handling.
				d->metaData->addMetaData_string(Property::Composer, v_gd3[6]);
			}
			if (line_count >= 9 && !v_gd3[8].empty()) {
				// Parse the release date.
				// NOTE: Only year is supported.
				int year;
				char chr;
				int s = sscanf(v_gd3[8].c_str(), "%04d%c", &year, &chr);
				if (s == 1 || (s == 2 && (chr == '-' || chr == '/'))) {
					// Year seems to be valid.
					// Make sure the number is acceptable:
					// - No negatives.
					// - Four-digit only. (lol Y10K)
					if (year >= 0 && year < 10000) {
						d->metaData->addMetaData_uint(Property::ReleaseYear, (unsigned int)year);
					}
				}
			}
			/*if (line_count >= 10 && !v_gd3[9].empty()) {
				// FIXME: No property for this...
				d->metaData->addMetaData_string(Property::VGMRipper, v_gd3[9]);
			}*/
			if (line_count >= 11 && !v_gd3[10].empty()) {
				d->metaData->addMetaData_string(Property::Comment, v_gd3[10]);
			}
		}
	}

	// Finished reading the metadata.
	return static_cast<int>(d->metaData->count());
}

}