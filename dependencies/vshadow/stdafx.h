// SPDX-License-Identifier: MIT
//
// This file is derived from the VShadow sample code in Microsoft's official
// Windows-classic-samples repository at commit
// d59e5f1dc9c768615e4e1ab1f0f009e6a3ed747c.
//
// The LICENSE file in the root of Microsoft's Windows-classic-samples
// repository grants permission free of charge, to any person obtaining a copy
// of the software and associated documentation files, to deal in the software
// without restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies of the
// software, and to permit persons to whom the software is furnished to do so,
// subject to the conditions stated in Microsoft's LICENSE file.
//
// The original VShadow sample contains no separate license terms that restrict
// the above-described grant. Microsoft's repository-root LICENSE therefore
// authorizes this adaptation and redistribution, subject to the conditions
// stated in Microsoft's LICENSE file.
//
// A copy of Microsoft's LICENSE file is included in this directory.
//
/////////////////////////////////////////////////////////////////////////
//
// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once


// General includes
#include <windows.h>
#include <winbase.h>

// _ASSERTE declaration (used by ATL) and otehr macros
#include "macros.h"



#include <iostream>
#include <tchar.h>
#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS  // some CString constructors will be explicit

// ATL includes
#pragma warning( disable: 4189 )    // disable local variable is initialized but not referenced
#include <atlbase.h>

// STL includes
#include <vector>
#include <map>
#include <algorithm>
#include <string>
#include <fstream>
using namespace std;   

// Used for safe string manipulation
#include <strsafe.h>

#include "shadow.h"

