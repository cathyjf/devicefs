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
// The original Microsoft source header follows.
//
/////////////////////////////////////////////////////////////////////////
// Copyright © Microsoft Corporation. All rights reserved.
// 
//  This file may contain preliminary information or inaccuracies, 
//  and may not correctly represent any associated Microsoft 
//  Product as commercially released. All Materials are provided entirely 
//  “AS IS.” To the extent permitted by law, MICROSOFT MAKES NO 
//  WARRANTY OF ANY KIND, DISCLAIMS ALL EXPRESS, IMPLIED AND STATUTORY 
//  WARRANTIES, AND ASSUMES NO LIABILITY TO YOU FOR ANY DAMAGES OF 
//  ANY TYPE IN CONNECTION WITH THESE MATERIALS OR ANY INTELLECTUAL PROPERTY IN THEM. 
// 


#pragma once


/////////////////////////////////////////////////////////////////////////
//  Generic tracing/logger class
//


// Very simple tracing/logging class 
class FunctionTracer
{
public:
    FunctionTracer(wstring fileName, INT lineNumber, wstring functionName);
    ~FunctionTracer();
    
    // tracing routine
    void Trace(wstring file, int line, wstring functionName, wstring format, ...);
    
    // console logging routine
    void WriteLine(wstring format, ...);
    
    // Converts a HRESULT into a printable message
    static wstring  HResult2String(HRESULT hrError);

    // Enables tracing
    [[deprecated]] static void EnableTracingMode();

private:

    //
    //  Data members
    //

    static bool m_traceEnabled;

    wstring     m_fileName;
    int         m_lineNumber;
    wstring     m_functionName;

};

