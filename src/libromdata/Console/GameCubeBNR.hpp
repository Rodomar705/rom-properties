/***************************************************************************
 * ROM Properties Page shell extension. (libromdata)                       *
 * GameCubeBNR.hpp: Nintendo GameCube banner reader.                       *
 *                                                                         *
 * Copyright (c) 2016-2018 by David Korth.                                 *
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

#ifndef __ROMPROPERTIES_LIBROMDATA_GAMECUBEBNR_HPP__
#define __ROMPROPERTIES_LIBROMDATA_GAMECUBEBNR_HPP__

#include "librpbase/RomData.hpp"
#include "gcn_banner.h"

namespace LibRomData {

ROMDATA_DECL_BEGIN(GameCubeBNR)
ROMDATA_DECL_METADATA()
ROMDATA_DECL_IMGSUPPORT()
ROMDATA_DECL_IMGPF()
ROMDATA_DECL_IMGINT()

	public:
		/** GameCubeBNR accessors. **/

		/**
		 * Get the gcn_banner_comment_t.
		 *
		 * For BNR2, this returns the comment that most closely
		 * matches the system language.
		 *
		 * return gcn_banner_comment_t, or nullptr on error.
		 */
		const gcn_banner_comment_t *getComment(void) const;

ROMDATA_DECL_END()

}

#endif /* __ROMPROPERTIES_LIBROMDATA_GAMECUBEBNR_HPP__ */
