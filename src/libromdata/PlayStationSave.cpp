/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * PlayStationSave.hpp: Sony PlayStation save file reader.                 *
 *                                                                         *
 * Copyright (c) 2016 by David Korth.                                      *
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
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, write to the Free Software Foundation, Inc., *
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.           *
 ***************************************************************************/

// References:
// - http://www.psdevwiki.com/ps3/Game_Saves#Game_Saves_PS1
// - http://problemkaputt.de/psx-spx.htm

#include "PlayStationSave.hpp"
#include "RomData_p.hpp"

#include "ps1_structs.h"

#include "common.h"
#include "byteswap.h"
#include "TextFuncs.hpp"
#include "file/IRpFile.hpp"

#include "img/rp_image.hpp"
#include "img/ImageDecoder.hpp"
#include "img/IconAnimData.hpp"

// C includes. (C++ namespace)
#include <cassert>
#include <cstring>
#include <cstddef>

// C++ includes.
#include <algorithm>
#include <vector>
using std::vector;

namespace LibRomData {

class PlayStationSavePrivate : public RomDataPrivate
{
	public:
		PlayStationSavePrivate(PlayStationSave *q, IRpFile *file);

	private:
		typedef RomDataPrivate super;
		PlayStationSavePrivate(const PlayStationSavePrivate &other);
		PlayStationSavePrivate &operator=(const PlayStationSavePrivate &other);

	public:
		/** RomFields **/

		// ROM fields.
		static const struct RomFields::Desc ps1_fields[];

	public:
		// Save file type.
		enum SaveType {
			SAVE_TYPE_UNKNOWN = -1,	// Unknown save type.

			SAVE_TYPE_PSV = 0,	// PS1 on PS3 individual save file.
		};
		int saveType;

	public:
		// Save file header. (PSV format)
		// NOTE: Must be byteswapped on access.
		PS1_PSV_Header psvHeader;

		// Animated icon data.
		// NOTE: The first frame is owned by the RomData superclass.
		IconAnimData *iconAnimData;

		/**
		 * Load the save file's icons.
		 *
		 * This will load all of the animated icon frames,
		 * though only the first frame will be returned.
		 *
		 * @return Icon, or nullptr on error.
		 */
		rp_image *loadIcon(void);
};

/** PlayStationSavePrivate **/

// ROM fields.
// TODO: Add more.
const struct RomFields::Desc PlayStationSavePrivate::ps1_fields[] = {
	{_RP("Filename"), RomFields::RFT_STRING, {nullptr}},
	{_RP("Description"), RomFields::RFT_STRING, {nullptr}},
};

PlayStationSavePrivate::PlayStationSavePrivate(PlayStationSave *q, IRpFile *file)
	: super(q, file, ps1_fields, ARRAY_SIZE(ps1_fields))
	, saveType(SAVE_TYPE_UNKNOWN)
	, iconAnimData(nullptr)
{ }

/**
 * Load the save file's icons.
 *
 * This will load all of the animated icon frames,
 * though only the first frame will be returned.
 *
 * @return Icon, or nullptr on error.
 */
rp_image *PlayStationSavePrivate::loadIcon(void)
{
	if (iconAnimData) {
		// Icon has already been loaded.
		return const_cast<rp_image*>(iconAnimData->frames[0]);
	}

	if (saveType != SAVE_TYPE_PSV) {
		// Only PSV (PS1 on PS3) is supported right now.
		return nullptr;
	}

	// Determine how many frames need to be decoded.
	int frames;
	int delay;
	switch (psvHeader.sc.icon_flag) {
		case PS1_SC_ICON_NONE:
		default:
			// No frames.
			return nullptr;

		case PS1_SC_ICON_STATIC:
		case PS1_SC_ICON_ALT_STATIC:
			// One frame.
			frames = 1;
			delay = 0;
			break;

		case PS1_SC_ICON_ANIM_2:
		case PS1_SC_ICON_ALT_ANIM_2:
			// Two frames.
			// Icon delay is 16 PAL frames.
			frames = 2;
			delay = 16 * 1000 / 50;
			break;

		case PS1_SC_ICON_ANIM_3:
		case PS1_SC_ICON_ALT_ANIM_3:
			// Three frames.
			// Icon delay is 11 PAL frames.
			frames = 3;
			delay = 11 * 1000 / 50;
			break;
	}

	this->iconAnimData = new IconAnimData();
	iconAnimData->count = frames;
	iconAnimData->seq_count = frames;

	// Decode the icon frames.
	for (int i = 0; i < frames; i++) {
		iconAnimData->delays[i] = delay;
		iconAnimData->seq_index[i] = i;

		// Icon format is linear 16x16 4bpp with RGB555 palette.
		iconAnimData->frames[i] = ImageDecoder::fromPS1_CI4(16, 16,
			psvHeader.sc.icon_data[i], sizeof(psvHeader.sc.icon_data[i]),
			psvHeader.sc.icon_pal, sizeof(psvHeader.sc.icon_pal));
	}


	// Return the first frame.
	return const_cast<rp_image*>(iconAnimData->frames[0]);
}

/** PlayStationSave **/

/**
 * Read a PlayStation save file.
 *
 * A save file must be opened by the caller. The file handle
 * will be dup()'d and must be kept open in order to load
 * data from the ROM image.
 *
 * To close the file, either delete this object or call close().
 *
 * NOTE: Check isValid() to determine if this is a valid ROM.
 *
 * @param file Open ROM image.
 */
PlayStationSave::PlayStationSave(IRpFile *file)
	: super(new PlayStationSavePrivate(this, file))
{
	// This class handles save files.
	RP_D(PlayStationSave);
	d->fileType = FTYPE_SAVE_FILE;

	if (!d->file) {
		// Could not dup() the file handle.
		return;
	}

	// Read the save file header.
	uint8_t header[1024];
	d->file->rewind();
	size_t size = d->file->read(&header, sizeof(header));
	if (size != sizeof(header))
		return;

	// Check if this ROM image is supported.
	DetectInfo info;
	info.header.addr = 0;
	info.header.size = sizeof(header);
	info.header.pData = header;
	info.ext = nullptr;	// Not needed for PS1.
	info.szFile = 0;	// Not needed for PS1.
	d->saveType = isRomSupported(&info);

	switch (d->saveType) {
		case PlayStationSavePrivate::SAVE_TYPE_PSV:
			// PSV (PS1 on PS3)
			// Save the header for later.
			memcpy(&d->psvHeader, header, sizeof(d->psvHeader));
			break;

		default:
			// Unknown save type.
			d->saveType = PlayStationSavePrivate::SAVE_TYPE_UNKNOWN;
			return;
	}

	d->isValid = true;
}

/**
 * Is a ROM image supported by this class?
 * @param info DetectInfo containing ROM detection information.
 * @return Class-specific system ID (>= 0) if supported; -1 if not.
 */
int PlayStationSave::isRomSupported_static(const DetectInfo *info)
{
	// NOTE: Only PSV is supported right now.
	assert(info != nullptr);
	assert(info->header.pData != nullptr);
	assert(info->header.addr == 0);
	if (!info || !info->header.pData ||
	    info->header.addr != 0 ||
	    info->header.size < sizeof(PS1_PSV_Header))
	{
		// Either no detection information was specified,
		// or the header is too small.
		return -1;
	}

	const PS1_PSV_Header *saveHeader =
		reinterpret_cast<const PS1_PSV_Header*>(info->header.pData);

	// Check the PSV magic.
	static const char psv_magic[8] = {
		0x00, 0x56, 0x53, 0x50, 0x00, 0x00, 0x00, 0x00
	};
	if (memcmp(saveHeader->magic, psv_magic, sizeof(psv_magic)) != 0) {
		// PSV magic is incorrect.
		return -1;
	}

	// Check the SC struct magic.
	static const char sc_magic[2] = {'S','C'};
	if (memcmp(saveHeader->sc.magic, sc_magic, sizeof(sc_magic)) != 0) {
		// SC magic is incorrect.
		return -1;
	}

	// This is a PSV (PS1 on PS3) save file.
	return PlayStationSavePrivate::SAVE_TYPE_PSV;
}

/**
 * Is a ROM image supported by this object?
 * @param info DetectInfo containing ROM detection information.
 * @return Class-specific system ID (>= 0) if supported; -1 if not.
 */
int PlayStationSave::isRomSupported(const DetectInfo *info) const
{
	return isRomSupported_static(info);
}

/**
 * Get the name of the system the loaded ROM is designed for.
 * @param type System name type. (See the SystemName enum.)
 * @return System name, or nullptr if type is invalid.
 */
const rp_char *PlayStationSave::systemName(uint32_t type) const
{
	RP_D(const PlayStationSave);
	if (!d->isValid || !isSystemNameTypeValid(type))
		return nullptr;

	// Bits 0-1: Type. (short, long, abbreviation)
	static const rp_char *const sysNames[4] = {
		// TODO: PS1 or PSX?
		_RP("Sony PlayStation"), _RP("PlayStation"), _RP("PS1"), nullptr
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
 * NOTE 2: The strings in the std::vector should *not*
 * be freed by the caller.
 *
 * @return List of all supported file extensions.
 */
vector<const rp_char*> PlayStationSave::supportedFileExtensions_static(void)
{
	static const rp_char *const exts[] = {
		_RP(".psv"),
		// TOOD: More formats?
	};
	return vector<const rp_char*>(exts, exts + ARRAY_SIZE(exts));
}

/**
 * Get a list of all supported file extensions.
 * This is to be used for file type registration;
 * subclasses don't explicitly check the extension.
 *
 * NOTE: The extensions include the leading dot,
 * e.g. ".bin" instead of "bin".
 *
 * NOTE 2: The strings in the std::vector should *not*
 * be freed by the caller.
 *
 * @return List of all supported file extensions.
 */
vector<const rp_char*> PlayStationSave::supportedFileExtensions(void) const
{
	return supportedFileExtensions_static();
}

/**
 * Get a bitfield of image types this class can retrieve.
 * @return Bitfield of supported image types. (ImageTypesBF)
 */
uint32_t PlayStationSave::supportedImageTypes_static(void)
{
	return IMGBF_INT_ICON;
}

/**
 * Get a bitfield of image types this class can retrieve.
 * @return Bitfield of supported image types. (ImageTypesBF)
 */
uint32_t PlayStationSave::supportedImageTypes(void) const
{
	return supportedImageTypes_static();
}

/**
 * Load field data.
 * Called by RomData::fields() if the field data hasn't been loaded yet.
 * @return Number of fields read on success; negative POSIX error code on error.
 */
int PlayStationSave::loadFieldData(void)
{
	RP_D(PlayStationSave);
	if (d->fields->isDataLoaded()) {
		// Field data *has* been loaded...
		return 0;
	} else if (!d->file) {
		// File isn't open.
		return -EBADF;
	} else if (!d->isValid) {
		// ROM image isn't valid.
		return -EIO;
	}

	// PSV (PS1 on PS3) save file header.
	const PS1_PSV_Header *psvHeader = &d->psvHeader;

	// Filename.
	d->fields->addData_string(
		cp1252_sjis_to_rp_string(psvHeader->filename, sizeof(psvHeader->filename)));

	// Description.
	d->fields->addData_string(
		cp1252_sjis_to_rp_string(psvHeader->sc.title, sizeof(psvHeader->sc.title)));

	// TODO: Moar fields.

	// Finished reading the field data.
	return (int)d->fields->count();
}

/**
 * Load an internal image.
 * Called by RomData::image() if the image data hasn't been loaded yet.
 * @param imageType Image type to load.
 * @return 0 on success; negative POSIX error code on error.
 */
int PlayStationSave::loadInternalImage(ImageType imageType)
{
	assert(imageType >= IMG_INT_MIN && imageType <= IMG_INT_MAX);
	if (imageType < IMG_INT_MIN || imageType > IMG_INT_MAX) {
		// ImageType is out of range.
		return -ERANGE;
	}

	RP_D(PlayStationSave);
	if (d->images[imageType]) {
		// Icon *has* been loaded...
		return 0;
	} else if (!d->file) {
		// File isn't open.
		return -EBADF;
	} else if (!d->isValid) {
		// ROM image isn't valid.
		return -EIO;
	}

	// Check for supported image types.
	if (imageType != IMG_INT_ICON) {
		// Only IMG_INT_ICON is supported by DS.
		return -ENOENT;
	}

	// Use nearest-neighbor scaling when resizing.
	d->imgpf[imageType] = IMGPF_RESCALE_NEAREST;
	d->images[imageType] = d->loadIcon();
	if (d->iconAnimData && d->iconAnimData->count > 1) {
		// Animated icon.
		d->imgpf[imageType] |= IMGPF_ICON_ANIMATED;
	}

	// TODO: -ENOENT if the file doesn't actually have an icon.
	return (d->images[imageType] != nullptr ? 0 : -EIO);
}

/**
 * Get the animated icon data.
 *
 * Check imgpf for IMGPF_ICON_ANIMATED first to see if this
 * object has an animated icon.
 *
 * @return Animated icon data, or nullptr if no animated icon is present.
 */
const IconAnimData *PlayStationSave::iconAnimData(void) const
{
	RP_D(const PlayStationSave);
	if (!d->iconAnimData) {
		// Load the icon.
		if (!const_cast<PlayStationSavePrivate*>(d)->loadIcon()) {
			// Error loading the icon.
			return nullptr;
		}
		if (!d->iconAnimData) {
			// Still no icon...
			return nullptr;
		}
	}

	if (d->iconAnimData->count <= 1) {
		// Not an animated icon.
		return nullptr;
	}

	// Return the icon animation data.
	return d->iconAnimData;
}

}
