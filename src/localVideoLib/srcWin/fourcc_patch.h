/*****************************************************************************
 *
 * fourcc_patch.h
 *  Part of videoLib integration with USB/build-in camera for Windows
 *
 *****************************************************************************
 *
 * Copyright 2013-2022 Sighthound, Inc.
 *
 * Licensed under the GNU GPLv3 license found at
 * https://www.gnu.org/licenses/gpl-3.0.txt
 *
 * Alternative licensing available from Sighthound, Inc.
 * by emailing opensource@sighthound.com
 *
 * This file is part of the Sighthound Video project which can be found at
 * https://github.url/thing
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; using version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 *****************************************************************************/


// FOURCCMap
//
// provides a mapping between old-style multimedia format DWORDs
// and new-style GUIDs.
//
// A range of 4 billion GUIDs has been allocated to ensure that this
// mapping can be done straightforwardly one-to-one in both directions.
//
// January 95


#ifndef __FOURCC__
#define __FOURCC__


// Multimedia format types are marked with DWORDs built from four 8-bit
// chars and known as FOURCCs. New multimedia AM_MEDIA_TYPE definitions include
// a subtype GUID. In order to simplify the mapping, GUIDs in the range:
//    XXXXXXXX-0000-0010-8000-00AA00389B71
// are reserved for FOURCCs.

class FOURCCMap : public GUID
{

public:
    FOURCCMap();
    FOURCCMap(DWORD Fourcc);
    FOURCCMap(const GUID *);


    DWORD GetFOURCC(void);
    void SetFOURCC(DWORD fourcc);
    void SetFOURCC(const GUID *);

private:
    void InitGUID();
};

#define GUID_Data2      0
#define GUID_Data3     0x10
#define GUID_Data4_1   0xaa000080
#define GUID_Data4_2   0x719b3800

inline void
FOURCCMap::InitGUID() {
    Data2 = GUID_Data2;
    Data3 = GUID_Data3;
    ((DWORD *)Data4)[0] = GUID_Data4_1;
    ((DWORD *)Data4)[1] = GUID_Data4_2;
}

inline
FOURCCMap::FOURCCMap() {
    InitGUID();
    SetFOURCC( DWORD(0));
}

inline
FOURCCMap::FOURCCMap(DWORD fourcc)
{
    InitGUID();
    SetFOURCC(fourcc);
}

inline
FOURCCMap::FOURCCMap(const GUID * pGuid)
{
    InitGUID();
    SetFOURCC(pGuid);
}

inline void
FOURCCMap::SetFOURCC(const GUID * pGuid)
{
    FOURCCMap * p = (FOURCCMap*) pGuid;
    SetFOURCC(p->GetFOURCC());
}

inline void
FOURCCMap::SetFOURCC(DWORD fourcc)
{
    Data1 = fourcc;
}

inline DWORD
FOURCCMap::GetFOURCC(void)
{
    return Data1;
}

#endif /* __FOURCC__ */

