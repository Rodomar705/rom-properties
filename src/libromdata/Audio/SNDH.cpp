/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * SNDH.hpp: Atari ST SNDH audio reader.                                   *
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

// Reference: http://sndh.atari.org/fileformat.php
// NOTE: The header format consists of tags that may be in any order,
// so we don't have a structs file.

#include "SNDH.hpp"
#include "librpbase/RomData_p.hpp"

// librpbase
#include "librpbase/common.h"
#include "librpbase/byteswap.h"
#include "librpbase/TextFuncs.hpp"
#include "librpbase/file/IRpFile.hpp"
#include "libi18n/i18n.h"
using namespace LibRpBase;

// C includes. (C++ namespace)
#include "librpbase/ctypex.h"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <ctime>

// C++ includes.
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
using std::pair;
using std::string;
using std::unique_ptr;
using std::unordered_map;
using std::vector;

namespace LibRomData {

ROMDATA_IMPL(SNDH)

class SNDHPrivate : public RomDataPrivate
{
	public:
		SNDHPrivate(SNDH *q, IRpFile *file);

	private:
		typedef RomDataPrivate super;
		RP_DISABLE_COPY(SNDHPrivate)

	public:
		// Parsed tags.
		struct TagData {
			bool tags_read;		// True if tags were read successfully.

			string title;		// Song title.
			string composer;	// Composer name.
			string ripper;		// Ripper name.
			string converter;	// Converter name.

			unsigned int subtunes;		// Subtune count. (If 0 or 1, entire file is one song.)
							// NOTE: 0 (or missing) means SNDHv1; 1 means SNDHv2.
			unsigned int vblank_freq;	// VBlank frequency. (50/60)
			unsigned int timer_freq[4];	// Timer frequencies. (A, B, C, D) [0 if not specified]
			unsigned int year;		// Year of release.
			unsigned int def_subtune;	// Default subtune.

			// TODO: Use std::pair<>?
			// The SNDH format uses separate tags for each, though...
			vector<string> subtune_names;		// Subtune names.
			vector<unsigned int> subtune_lengths;	// Subtune lengths, in seconds.

			TagData() : tags_read(false), subtunes(0), vblank_freq(0), year(0), def_subtune(0)
			{
				// Clear the timer frequencies.
				memset(&timer_freq, 0, sizeof(timer_freq));
			}
		};

		/**
		 * Read a NULL-terminated ASCII string from an arbitrary binary buffer.
		 * @param p	[in/out] String pointer. (will be adjusted to point after the NULL terminator)
		 * @param p_end	[in] End of buffer pointer.
		 * @param p_err	[out] Set to true if the string is out of bounds.
		 * @return ASCII string.
		 */
		static string readStrFromBuffer(const uint8_t **p, const uint8_t *p_end, bool *p_err);

		/**
		 * Read a NULL-terminated unsigned ASCII number from an arbitrary binary buffer.
		 * @param p	[in/out] String pointer. (will be adjusted to point after the NULL terminator)
		 * @param p_end	[in] End of buffer pointer.
		 * @param p_err	[out] Set to true if the string is out of bounds.
		 * @return Unsigned ASCII number.
		 */
		static unsigned int readAsciiNumberFromBuffer(const uint8_t **p, const uint8_t *p_end, bool *p_err);

		/**
		 * Parse the tags from the open SNDH file.
		 * @return TagData object.
		 */
		TagData parseTags(void);
};

/** SNDHPrivate **/

SNDHPrivate::SNDHPrivate(SNDH *q, IRpFile *file)
	: super(q, file)
{ }

/**
 * Read a NULL-terminated ASCII string from an arbitrary binary buffer.
 * @param p	[in/out] String pointer. (will be adjusted to point after the NULL terminator)
 * @param p_end	[in] End of buffer pointer.
 * @param p_err	[out] Set to true if the string is out of bounds.
 * @return ASCII string.
 */
string SNDHPrivate::readStrFromBuffer(const uint8_t **p, const uint8_t *p_end, bool *p_err)
{
	if (*p >= p_end) {
		// Out of bounds.
		*p_err = true;
		return string();
	}

	const uint8_t *const s_end = reinterpret_cast<const uint8_t*>(memchr(*p, 0, p_end-*p));
	if (!s_end) {
		// Out of bounds.
		*p_err = true;
		return string();
	}

	*p_err = false;
	if (s_end > *p) {
		// TODO: Convert from ATASCII, not Latin-1.
		string ret = latin1_to_utf8(reinterpret_cast<const char*>(*p), (int)(s_end-*p));
		// Skip the string, then add one for the NULL terminator.
		*p = s_end + 1;
		return ret;
	}

	// Empty string.
	return string();
}

/**
 * Read a NULL-terminated unsigned ASCII number from an arbitrary binary buffer.
 * @param p	[in/out] String pointer. (will be adjusted to point after the NULL terminator)
 * @param p_end	[in] End of buffer pointer.
 * @param p_err	[out] Set to true if the string is out of bounds.
 * @return Unsigned ASCII number.
 */
unsigned int SNDHPrivate::readAsciiNumberFromBuffer(const uint8_t **p, const uint8_t *p_end, bool *p_err)
{
	if (*p >= p_end) {
		// Out of bounds.
		*p_err = true;
		return 0;
	}

	char *endptr;
	unsigned int ret = (unsigned int)strtoul(reinterpret_cast<const char*>(*p), &endptr, 10);
	if (endptr >= reinterpret_cast<const char*>(p_end) || *endptr != '\0') {
		// Invalid value.
		// NOTE: Return the parsed number anyway, in case it's still useful.
		// For exmaple, 'YEAR' tags might have "1995/2013".
		// See: Modmate/almoST_real_(ENtRACte).sndh
		*p_err = true;
		return ret;
	}
	// endptr is the NULL terminator, so go one past that.
	*p = reinterpret_cast<const uint8_t*>(endptr) + 1;
	return ret;
}

/**
 * Parse the tags from the open SNDH file.
 * @return TagData object.
 */
SNDHPrivate::TagData SNDHPrivate::parseTags(void)
{
	TagData tags;

	// FIXME: May be compressed with "Pack-Ice".
	// https://sourceforge.net/projects/sc68/files/unice68/

	// Read up to 4 KB from the beginning of the file.
	// TODO: Support larger headers?
	static const size_t HEADER_SIZE = 4096;
	unique_ptr<uint8_t[]> header(new uint8_t[HEADER_SIZE+1]);
	size_t sz = file->seekAndRead(0, header.get(), HEADER_SIZE);
	if (sz < 16) {
		// Not enough data for "SNDH" and "HDNS".
		return tags;
	}
	header[HEADER_SIZE] = 0;	// ensure NULL-termination

	// Verify the header.
	// NOTE: SNDH is defined as using CRLF line endings,
	// but we'll allow LF line endings too.
	const uint8_t *p = header.get();
	const uint8_t *const p_end = header.get() + HEADER_SIZE;
	if (memcmp(&header[12], "SNDH", 4) != 0) {
		// Not SNDH.
		return tags;
	}
	p += 16;

	// NOTE: Strings are in ASCII.
	// TODO: Add ascii_to_utf8(). For now, using latin1_to_utf8().
	bool err = false;
	while (p < p_end) {
		// Check for 32-bit tags.
		// NOTE: This might not be aligned, so we'll need to handle
		// alignment properly later.
		uint32_t tag = be32_to_cpu(*(reinterpret_cast<const uint32_t*>(p)));
		bool is32 = true;
		switch (tag) {
			case 'TITL':
				// Song title.
				p += 4;
				tags.title = readStrFromBuffer(&p, p_end, &err);
				assert(!err);
				if (err) {
					// An error occurred.
					p = p_end;
				}
				break;

			case 'COMM':
				// Composer.
				p += 4;
				tags.composer = readStrFromBuffer(&p, p_end, &err);
				assert(!err);
				if (err) {
					// An error occurred.
					p = p_end;
				}
				break;

			case 'RIPP':
				// Ripper.
				p += 4;
				tags.ripper = readStrFromBuffer(&p, p_end, &err);
				assert(!err);
				if (err) {
					// An error occurred.
					p = p_end;
				}
				break;

			case 'CONV':
				// Converter.
				p += 4;
				tags.converter = readStrFromBuffer(&p, p_end, &err);
				assert(!err);
				if (err) {
					// An error occurred.
					p = p_end;
				}
				break;

			case 'YEAR': {
				// Year of release.
				// String uses ASCII digits, so use strtoul().
				// FIXME: Modmate/almoST_real_(ENtRACte).sndh has "1995/2013".
				// Ignoring errors for now.
				p += 4;
				tags.year = readAsciiNumberFromBuffer(&p, p_end, &err);
				if (err) {
					// An error occurred.
					if (tags.year != 0) {
						// FIXME: Might be two years, e.g. "1995/2013".
						// This might also be "198x".
						// Ignore the non-numeric portion for now.
						const uint8_t *const p_nul = reinterpret_cast<const uint8_t*>(memchr(p, 0, p_end - p));
						if (p_nul) {
							err = false;	// Ignore this error for now.
							p = p_nul + 1;
						} else {
							p = p_end;
						}
					} else {
						// Invalid year, probably.
						p = p_end;
					}
 				}
 				break;
 			}

			case '!#SN':
			case '!#ST': {
				// Subtune names.

				// NOTE: If subtune count is 0 (no '##' tag), this is SNDHv1,
				// which doesn't support subtunes. Handle it as a single subtune.
				const unsigned int subtunes = (tags.subtunes > 0 ? tags.subtunes : 1);

				// The following WORDs are offsets from the tag,
				// and they point to NULL-terminated strings.
				// The next tag is immediately after the last string.
				assert(tags.subtune_names.empty());
				if (!tags.subtune_names.empty()) {
					// We already have subtune names.
					// This means there's a duplicate '!#SN' tag.
					p = p_end;
					break;
				}

				const uint16_t *p_tbl = reinterpret_cast<const uint16_t*>(p + 4);
				const uint8_t *p_next = nullptr;
				for (unsigned int i = 0; i < subtunes; i++, p_tbl++) {
					const uint8_t *p_str = p + be16_to_cpu(*p_tbl);
					string str = readStrFromBuffer(&p_str, p_end, &err);
					//assert(!err);	// FIXME: Breaks Johansen_Benny/Yahtzee.sndh.
					if (err) {
						// An error occured.
						break;
					}
					tags.subtune_names.push_back(std::move(str));

					if (p_str > p_next) {
						// This string is the farthest ahead so far.
						p_next = p_str;
					}
				}

				//assert(!err);	// FIXME: Breaks Johansen_Benny/Yahtzee.sndh.
				if (err) {
					// An error occurred.
					p = p_end;
					tags.subtune_names.clear();
				} else {
					// p_next is the next byte to read.
					// NOTE: http://sndh.atari.org/fileformat.php says it should be 16-bit aligned.
					p = p_next;
				}
				break;
			}

			case 'TIME': {
				// Subtune lengths, in seconds.
				// NOTE: This field is OPTIONAL.
				// Count_Zero/Decade_Demo_Quartet.sndh has '!#SN', but not 'TIME'.

				// NOTE: If subtune count is 0, this is SNDHv1,
				// which only supports one subtune.
				const unsigned int subtunes = (tags.subtunes > 0 ? tags.subtunes : 1);

				// Immediately following the tag is a table of WORDs,
				// with one element per subtune.
				const uint8_t *const p_next = p + 4 + (subtunes * 2);
				assert(p_next <= p_end);
				if (p_next > p_end) {
					// Out of bounds.
					p = p_end;
					break;
				}

				const uint16_t *p_tbl = reinterpret_cast<const uint16_t*>(p + 4);
				tags.subtune_lengths.resize(subtunes);
				for (auto iter = tags.subtune_lengths.begin(); iter != tags.subtune_lengths.end(); ++iter, p_tbl++) {
					*iter = be16_to_cpu(*p_tbl);
				}

				p = p_next;
				break;
			}

			case 'HDNS':
				// End of SNDH header.
				p = p_end;
				break;

			default:
				// Need to check for 16-bit tags next.
				is32 = false;
				break;
		}

		if (is32) {
			// A 32-bit tag was parsed.
			// Check the next tag.
			continue;
		}

		// Check for 16-bit tags.
		tag = be16_to_cpu(*(reinterpret_cast<const uint16_t*>(p)));
		switch (tag) {
			case '##':
				// # of subtunes.
				// String uses ASCII digits, so use strtoul().
				// NOTE: Digits might not be NULL-terminated,
				// so instead of using readAsciiNumberFromBuffer(),
				// parse the two digits manually.
				assert(p + 4 <= p_end);
				if (p + 4 > p_end) {
					// Out of bounds.
					p = p_end;
					break;
				}

				assert(ISDIGIT(p[2]));
				assert(ISDIGIT(p[3]));
				if (!ISDIGIT(p[2]) || !ISDIGIT(p[3])) {
					// Not digits.
					p = p_end;
					break;
				}

				tags.subtunes = ((p[2] - '0') * 10) + (p[3] - '0');
				p += 4;
				break;

			case '!V':
				// VBlank frequency.
				p += 2;
				tags.vblank_freq = readAsciiNumberFromBuffer(&p, p_end, &err);
				assert(!err);
				if (err) {
					// An error occurred.
					p = p_end;
				}
				break;

			case 'TA':
			case 'TB':
			case 'TC':
			case 'TD': {
				// Timer frequency.

				// Check for invalid digits after 'Tx'.
				// If present, this is probably the end of the header,
				// and the file is missing an HDNS tag.
				// See: Beast/Boring.sndh
				if (!ISDIGIT(p[2])) {
					// End of header.
					p = p_end;
					break;
				}

				const uint8_t idx = p[1] - 'A';
				p += 2;
				tags.timer_freq[idx] = readAsciiNumberFromBuffer(&p, p_end, &err);
				assert(!err);
				if (err) {
					// An error occurred.
					p = p_end;
				}
				break;
			}

			case '!#':
				// Default subtune.
				// NOTE: First subtune is 1, not 0.
				// TODO: Check that it doesn't exceed the subtune count?
				p += 2;
				tags.def_subtune = readAsciiNumberFromBuffer(&p, p_end, &err);
				assert(!err);
				if (err) {
					// An error occurred.
					p = p_end;
				}
				break;

			default:
				// Unsupported tag...
				// If this is a NULL byte or a space, find the next
				// non-NULL/non-space byte and continue.
				// Otherwise, it's an invalid tag, so stop processing.
				if (*p == 0 || *p == ' ') {
					while (p < p_end && (*p == 0 || *p == ' ')) {
						p++;
					}
				} else {
					// Invalid tag.
					p = p_end;
				}
				break;
		}
	}

	// Tags parsed.
	tags.tags_read = true;
	return tags;
}

/** SNDH **/

/**
 * Read an SNDH audio file.
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
SNDH::SNDH(IRpFile *file)
	: super(new SNDHPrivate(this, file))
{
	RP_D(SNDH);
	d->className = "SNDH";
	d->fileType = FTYPE_AUDIO_FILE;

	if (!d->file) {
		// Could not dup() the file handle.
		return;
	}

	// Read the SNDH header.
	uint8_t buf[16];
	d->file->rewind();
	size_t size = d->file->read(buf, sizeof(buf));
	if (size != sizeof(buf)) {
		delete d->file;
		d->file = nullptr;
		return;
	}

	// Check if this file is supported.
	DetectInfo info;
	info.header.addr = 0;
	info.header.size = sizeof(buf);
	info.header.pData = buf;
	info.ext = nullptr;	// Not needed for SNDH.
	info.szFile = 0;	// Not needed for SNDH.
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
int SNDH::isRomSupported_static(const DetectInfo *info)
{
	assert(info != nullptr);
	assert(info->header.pData != nullptr);
	assert(info->header.addr == 0);
	if (!info || !info->header.pData ||
	    info->header.addr != 0 ||
	    info->header.size < 16)
	{
		// Either no detection information was specified,
		// or the header is too small.
		return -1;
	}

	// Check for "SNDH" at offset 12.
	const uint32_t *const pData32 = reinterpret_cast<const uint32_t*>(info->header.pData);
	if (pData32[3] == cpu_to_be32('SNDH')) {
		// Found the SNDH magic number.
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
const char *SNDH::systemName(unsigned int type) const
{
	RP_D(const SNDH);
	if (!d->isValid || !isSystemNameTypeValid(type))
		return nullptr;

	// SNDH has the same name worldwide, so we can
	// ignore the region selection.
	static_assert(SYSNAME_TYPE_MASK == 3,
		"SNDH::systemName() array index optimization needs to be updated.");

	static const char *const sysNames[4] = {
		"Atari ST SNDH Audio",
		"SNDH",
		"SNDH",
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
const char *const *SNDH::supportedFileExtensions_static(void)
{
	static const char *const exts[] = {
		".sndh",

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
const char *const *SNDH::supportedMimeTypes_static(void)
{
	static const char *const mimeTypes[] = {
		// Unofficial MIME types.
		// TODO: Get these upstreamed on FreeDesktop.org.
		"audio/x-sndh",

		nullptr
	};
	return mimeTypes;
}

/**
 * Load field data.
 * Called by RomData::fields() if the field data hasn't been loaded yet.
 * @return Number of fields read on success; negative POSIX error code on error.
 */
int SNDH::loadFieldData(void)
{
	RP_D(SNDH);
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

	// Get the tags.
	SNDHPrivate::TagData tags = d->parseTags();
	if (!tags.tags_read) {
		// No tags.
		return 0;
	}

	// SNDH header.
	d->fields->reserve(4);	// Maximum of 4 fields.

	// NOTE: Some strings have trailing spaces.

	// Song title.
	if (!tags.title.empty()) {
		d->fields->addField_string(C_("SNDH", "Song Title"),
			tags.title, RomFields::STRF_TRIM_END);
	}

	// Composer.
	if (!tags.composer.empty()) {
		d->fields->addField_string(C_("SNDH", "Composer"),
			tags.composer, RomFields::STRF_TRIM_END);
	}

	// Ripper.
	if (!tags.ripper.empty()) {
		d->fields->addField_string(C_("SNDH", "Ripper"),
			tags.ripper, RomFields::STRF_TRIM_END);
	}

	// Converter.
	if (!tags.converter.empty()) {
		d->fields->addField_string(C_("SNDH", "Converter"),
			tags.converter, RomFields::STRF_TRIM_END);
	}

	// Year of release.
	if (tags.year != 0) {
		d->fields->addField_string_numeric(C_("SNDH", "Year of Release"), tags.year);
	}

	// Number of subtunes.
	// TODO: Omit this if it's 0 or 1?
	d->fields->addField_string_numeric(C_("SNDH", "# of Subtunes"),
		(tags.subtunes > 0) ? tags.subtunes : 1);

	// NOTE: Tag listing on http://sndh.atari.org/fileformat.php lists
	// VBL *after* timers, but "Calling method and speed" lists
	// VBL *before* timers. We'll list it before timers.

	// VBlank frequency.
	if (tags.vblank_freq != 0) {
		d->fields->addField_string(C_("SNDH", "VBlank Freq"),
			rp_sprintf(C_("SNDH", "%u Hz"), tags.vblank_freq));
	}

	// Timer frequencies.
	// TODO: Use RFT_LISTDATA?
	for (unsigned int i = 0; i < (unsigned int)ARRAY_SIZE(tags.timer_freq); i++) {
		if (tags.timer_freq[i] == 0)
			continue;

		d->fields->addField_string(
			rp_sprintf(C_("SNDH", "Timer %c Freq"), 'A'+i).c_str(),
			rp_sprintf(C_("SNDH", "%u Hz"), tags.timer_freq[i]));
	}

	// Default subtune.
	// NOTE: First subtune is 1, not 0.
	if (tags.subtunes > 1 && tags.def_subtune > 0) {
		d->fields->addField_string_numeric(C_("SMDH", "Default Subtune"), tags.def_subtune);
	}

	// Subtune list.
	// NOTE: We don't want to display the list if no subtune names are present
	// and we have a single subtune length, since that means we have only a
	// single song with a single duration.
	if (!tags.subtune_names.empty() || tags.subtune_lengths.size() > 1) {
		// NOTE: While most SMDH files have both '!#SN' and 'TIME',
		// some files might have only one or the other.
		// Example: Count_Zero/Decade_Demo_Quartet.sndh ('!#SN' only)
		bool has_SN = !tags.subtune_names.empty();
		bool has_TIME = !tags.subtune_lengths.empty();
		unsigned int col_count = 2 + (has_SN && has_TIME);

		// Some SNDH files have all zeroes for duration.
		// Example: Taylor_Nathan/180.sndh
		// If this is the case, and there are no names, don't bother showing the list.
		// TODO: Hide the third column if there are names but all zero durations?
		uint64_t duration_total = 0;

		const size_t count = std::max(tags.subtune_names.size(), tags.subtune_lengths.size());
		auto subtune_list = new vector<vector<string> >();
		subtune_list->resize(count);

		auto dest_iter = subtune_list->begin();
		unsigned int idx = 0;
		for (; dest_iter != subtune_list->end(); ++dest_iter, idx++) {
			vector<string> &data_row = *dest_iter;
			data_row.reserve(col_count);	// 2 or 3 fields per row.

			data_row.push_back(rp_sprintf("%u", idx+1));	// NOTE: First subtune is 1, not 0.
			if (has_SN) {
				if (idx < tags.subtune_names.size()) {
					data_row.push_back(tags.subtune_names.at(idx));
				} else {
					data_row.push_back(string());
				}
			}

			if (has_TIME) {
				if (idx < tags.subtune_lengths.size()) {
					// Format as m:ss.
					// TODO: Separate function?
					const uint32_t duration = tags.subtune_lengths.at(idx);
					duration_total += duration;
					const uint32_t min = duration / 60;
					const uint32_t sec = duration % 60;
					data_row.push_back(rp_sprintf("%u:%02u", min, sec));
				} else {
					data_row.push_back(string());
				}
			}
		}

		if (!has_SN && has_TIME && duration_total == 0) {
			// No durations. Don't bother showing the list.
			delete subtune_list;
		} else {
			static const char *subtune_list_hdr[3] = {
				NOP_C_("SNDH|SubtuneList", "#"),
				nullptr, nullptr
			};
			if (has_SN && has_TIME) {
				subtune_list_hdr[1] = NOP_C_("SNDH|SubtuneList", "Name");
				subtune_list_hdr[2] = NOP_C_("SNDH|SubtuneList", "Duration");
			} else if (has_SN) {
				subtune_list_hdr[1] = NOP_C_("SNDH|SubtuneList", "Name");
			} else if (has_TIME) {
				subtune_list_hdr[1] = NOP_C_("SNDH|SubtuneList", "Duration");
			} else {
				assert(!"Invalid combination of has_SN and has_TIME.");
				col_count = 1;
			}

			vector<string> *const v_subtune_list_hdr = RomFields::strArrayToVector_i18n(
				"SNDH|SubtuneList", subtune_list_hdr, col_count);
			d->fields->addField_listData("Subtune List", v_subtune_list_hdr, subtune_list);
		}
	} else if (tags.subtune_names.empty() && tags.subtune_lengths.size() == 1) {
		// No subtune names, but we have one subtune length.
		// This means it's the length of the entire song.

		// Format as m:ss.
		// TODO: Separate function?
		const uint32_t duration = tags.subtune_lengths.at(0);
		const uint32_t min = duration / 60;
		const uint32_t sec = duration % 60;

		d->fields->addField_string(C_("SNDH", "Duration"),
			rp_sprintf("%u:%02u", min, sec));
	}

	// Finished reading the field data.
	return static_cast<int>(d->fields->count());
}

}
