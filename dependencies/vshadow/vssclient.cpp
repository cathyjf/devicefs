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


// Main header
#include "shadow.h"
#include <mutex>
#include <objidl.h>



// Constructor
VssClient::VssClient()
{
    m_bCoInitializeCalled = false;
    m_hCancellationEvent = NULL;
    m_bSnapshotSetCreated = false;
    m_dwContext = VSS_CTX_BACKUP;
    m_latestSnapshotSetID = GUID_NULL;
    m_bDuringRestore = false;
}


// Destructor
VssClient::~VssClient()
{
    // Release the IVssBackupComponents interface 
    // WARNING: this must be done BEFORE calling CoUninitialize()
    m_pVssObject = NULL;
    
    // Call CoUninitialize if the CoInitialize was performed sucesfully
    if (m_bCoInitializeCalled)
        CoUninitialize();
}


// Initialize the COM infrastructure and the internal pointers
void VssClient::Initialize(DWORD dwContext, wstring xmlDoc, bool bDuringRestore, HANDLE hCancellationEvent)
{
    FunctionTracer ft(DBG_INFO);

    m_hCancellationEvent = hCancellationEvent;

    // Initialize COM 
    CHECK_COM( CoInitialize(NULL) );
    m_bCoInitializeCalled = true;

    // As instructed by Microsoft's guidelines, configure process-wide COM
    // behavior before exposing VSS callback interfaces. Masking an exception
    // there could leave the requester in a corrupted state.
    // See <https://learn.microsoft.com/en-us/windows/win32/vss/security-considerations-for-requestors>.
    static auto process_com_once = std::once_flag{};
    std::call_once(process_com_once, [&ft] {
        auto global_options = CComPtr<IGlobalOptions>{};
        CHECK_COM(global_options.CoCreateInstance(
            CLSID_GlobalOptions, nullptr, CLSCTX_INPROC_SERVER));
        CHECK_COM(global_options->Set(
            COMGLB_EXCEPTION_HANDLING,
            COMGLB_EXCEPTION_DONOT_HANDLE_ANY));

        CHECK_COM(
            CoInitializeSecurity(
                NULL,                           //  Allow *all* VSS writers to communicate back!
                -1,                             //  Default COM authentication service
                NULL,                           //  Default COM authorization service
                NULL,                           //  reserved parameter
                RPC_C_AUTHN_LEVEL_PKT_PRIVACY,  //  Strongest COM authentication level
                RPC_C_IMP_LEVEL_IMPERSONATE,    //  Minimal impersonation abilities
                NULL,                           //  Default COM authentication settings
                EOAC_DYNAMIC_CLOAKING,          //  Cloaking
                NULL                            //  Reserved parameter
                ) );
    });

    // Create the internal backup components object
    CHECK_COM( CreateVssBackupComponents(&m_pVssObject) );
    
    // We are during restore now?
    m_bDuringRestore = bDuringRestore;

    // Call either Initialize for backup or for restore
    if (m_bDuringRestore)
    {
        CHECK_COM(m_pVssObject->InitializeForRestore(CComBSTR(xmlDoc.c_str())))
    }
    else
    {
        // Initialize for backup
        if (xmlDoc.length() == 0)
            CHECK_COM(m_pVssObject->InitializeForBackup())
        else
            CHECK_COM(m_pVssObject->InitializeForBackup(CComBSTR(xmlDoc.c_str())))

        // Set the context, if different than the default context
        if (dwContext != VSS_CTX_BACKUP)
        {
            ft.WriteLine(L"- Setting the VSS context to: 0x%08lx", dwContext);
            CHECK_COM(m_pVssObject->SetContext(dwContext) );
        }

    }

    // Keep the context
    m_dwContext = dwContext;

    // Set various properties per backup components instance
    CHECK_COM(m_pVssObject->SetBackupState(true, true, VSS_BT_FULL, false));
}



// Waits for the completion of the asynchronous operation
bool VssClient::WaitAndCheckForAsyncOperation(
    IVssAsync* pAsync,
    bool bCancellable,
    bool bDeferCancellation)
{
    FunctionTracer ft(DBG_INFO);

    ft.WriteLine(L"(Waiting for the asynchronous operation to finish...)");

    HRESULT hrReturned = S_OK;
    bool bCancellationRequested = false;

    if (!bCancellable || (m_hCancellationEvent == NULL))
    {
        // Wait until the async operation finishes
        CHECK_COM(pAsync->Wait());

        // Check the result of the asynchronous operation
        CHECK_COM(pAsync->QueryStatus(&hrReturned, NULL));
    }
    else
    {
        do
        {
            CHECK_COM(pAsync->QueryStatus(&hrReturned, NULL));
            if (hrReturned != VSS_S_ASYNC_PENDING)
                break;

            DWORD dwWait = WaitForSingleObject(m_hCancellationEvent, 100);
            if ((dwWait == WAIT_OBJECT_0) && !bCancellationRequested)
            {
                CHECK_COM(pAsync->Cancel());
                bCancellationRequested = true;
            }
            else if (dwWait == WAIT_OBJECT_0)
            {
                Sleep(100);
            }
            else if (dwWait == WAIT_FAILED)
            {
                CHECK_WIN32_ERROR(GetLastError(), "WaitForSingleObject");
            }
        }
        while (true);
    }

    if (hrReturned == VSS_S_ASYNC_CANCELLED)
        throw(HRESULT_FROM_WIN32(ERROR_CANCELLED));

    // Check if the async operation succeeded...
    if(FAILED(hrReturned))
    {
        ft.WriteLine(L"Error during the last asynchronous operation.");
        ft.WriteLine(L"- Returned HRESULT = 0x%08lx", hrReturned);
        ft.WriteLine(L"- Error text: %s", FunctionTracer::HResult2String(hrReturned).c_str());
        throw(hrReturned);
    }

    if (bCancellable && (m_hCancellationEvent != NULL))
    {
        DWORD dwWait = WaitForSingleObject(m_hCancellationEvent, 0);
        if (dwWait == WAIT_OBJECT_0)
            bCancellationRequested = true;
        else if (dwWait == WAIT_FAILED)
            CHECK_WIN32_ERROR(GetLastError(), "WaitForSingleObject");
    }

    if (bCancellationRequested && !bDeferCancellation)
        throw(HRESULT_FROM_WIN32(ERROR_CANCELLED));

    return bCancellationRequested;
}





