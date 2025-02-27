/*
 * Copyright (c) 2013, Elmar Langholz
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#define UNICODE

#include <Windows.h>
#include <v8.h>
#include <node.h>
#include <string.h>
#include <stdlib.h>
#include <node_object_wrap.h>
#include "../DeviceINQ.h"
#include "BluetoothHelpers.h"

#include "bluetoothapis.h"
#pragma comment(lib, "Bthprops.lib")

using namespace std;
using namespace node;
using namespace v8;

// callback for BluetoothSdpEnumAttributes()

BOOL __stdcall callback(ULONG uAttribId, LPBYTE pValueStream, ULONG cbStreamSize, LPVOID pvParam)
{
    SDP_ELEMENT_DATA element;

    printf("uAttribId:\t%ul\n", uAttribId);
    printf("cbStreamSize:\t%ul\n ", cbStreamSize);

    if (BluetoothSdpGetElementData(pValueStream, cbStreamSize, &element) != ERROR_SUCCESS)
    {
        printf("BluetoothSdpGetElementData() failed with error code %ld\n", WSAGetLastError());
        return FALSE;
    }
    else
    {
        printf("*pValueStream:\t%u\n", *pValueStream);
        printf("element.data.string.value: \t%u\n\n", element.data.string.value);
        return TRUE;
    }
}

void DeviceINQ::EIO_SdpSearch(uv_work_t *req)
{
    sdp_baton_t *baton = static_cast<sdp_baton_t *>(req->data);

    // Default, no channel is found
    baton->channelID = -1;

    // Construct windows socket bluetooth variables
    DWORD flags = LUP_FLUSHCACHE | LUP_RETURN_ADDR;
    DWORD querySetSize = sizeof(WSAQUERYSET);
    WSAQUERYSET *querySet = (WSAQUERYSET *)malloc(querySetSize);
    if (querySet == nullptr)
    {
        return;
    }

    wchar_t wAddress[40];
    mbstowcs(wAddress, baton->address, sizeof(baton->address));
    LPWSTR lpwstrAddress = wAddress;

    ZeroMemory(querySet, querySetSize);
    querySet->dwSize = querySetSize;
    querySet->dwNameSpace = NS_BTH;
    querySet->lpServiceClassId = (LPGUID)&SerialPortServiceClass_UUID;
    querySet->dwNumberOfCsAddrs = 0;
    querySet->lpszContext = lpwstrAddress;

    // Initiate client device inquiry
    HANDLE lookupServiceHandle;
    int lookupServiceError = WSALookupServiceBegin(querySet, flags, &lookupServiceHandle);
    if (lookupServiceError != SOCKET_ERROR)
    {
        // Iterate over each found bluetooth service
        bool inquiryComplete = false;
        while (!inquiryComplete)
        {
            // For each bluetooth service retrieve its corresponding details
            lookupServiceError = WSALookupServiceNext(lookupServiceHandle, flags, &querySetSize, querySet);
            if (lookupServiceError != SOCKET_ERROR)
            {
                char address[19] = {0};
                SOCKADDR_BTH *bluetoothSocketAddress = (SOCKADDR_BTH *)querySet->lpcsaBuffer->RemoteAddr.lpSockaddr;
                baton->channelID = bluetoothSocketAddress->port;
                inquiryComplete = true;
            }
            else
            {
                int lookupServiceErrorNumber = WSAGetLastError();
                if (lookupServiceErrorNumber == WSAEFAULT)
                {
                    free(querySet);
                    querySet = (WSAQUERYSET *)malloc(querySetSize);
                    if (querySet == nullptr)
                    {
                        WSALookupServiceEnd(lookupServiceHandle);
                        return;
                    }
                }
                else if (lookupServiceErrorNumber == WSA_E_NO_MORE)
                {
                    // No more services where found
                    inquiryComplete = true;
                }
                else
                {
                    // Unhandled error
                    inquiryComplete = true;
                }
            }
        }
    }
    else
    {
        int lookupServiceErrorNumber = WSAGetLastError();
        if (lookupServiceErrorNumber != WSASERVICE_NOT_FOUND)
        {
            free(querySet);
            return;
        }
    }

    free(querySet);
    WSALookupServiceEnd(lookupServiceHandle);
}

void DeviceINQ::EIO_AfterSdpSearch(uv_work_t *req)
{
    Nan::HandleScope scope;

    sdp_baton_t *baton = static_cast<sdp_baton_t *>(req->data);

    Nan::TryCatch try_catch;

    Local<Value> argv[] = {
        Nan::New(baton->channelID)};

    Nan::Call(*baton->cb, 1, argv);

    if (try_catch.HasCaught())
    {
        Nan::FatalException(try_catch);
    }

    baton->inquire->Unref();
    delete baton->cb;
    delete baton;
    baton = nullptr;
}

void DeviceINQ::Init(Local<Object> target)
{
    Nan::HandleScope scope;

    Local<FunctionTemplate> t = Nan::New<FunctionTemplate>(New);

    t->InstanceTemplate()->SetInternalFieldCount(1);
    t->SetClassName(Nan::New("DeviceINQ").ToLocalChecked());

    Isolate *isolate = target->GetIsolate();
    Local<Context> ctx = isolate->GetCurrentContext();

    Nan::SetPrototypeMethod(t, "inquire", Inquire);
    Nan::SetPrototypeMethod(t, "findSerialPortChannel", SdpSearch);
    Nan::SetPrototypeMethod(t, "listPairedDevices", ListPairedDevices);
    target->Set(ctx, Nan::New("DeviceINQ").ToLocalChecked(), t->GetFunction(ctx).ToLocalChecked());
}

bt_inquiry DeviceINQ::doInquire()
{
    bt_inquiry inquiryResult;

    // Construct windows socket bluetooth variables
    DWORD flags = LUP_CONTAINERS | LUP_FLUSHCACHE | LUP_RETURN_TYPE | LUP_RES_SERVICE | LUP_RETURN_NAME | LUP_RETURN_ADDR | LUP_RETURN_BLOB;
    DWORD querySetSize = sizeof(WSAQUERYSET);
    WSAQUERYSET *querySet = (WSAQUERYSET *)malloc(querySetSize);

    // LPBLOB lpBLob;
    if (querySet == nullptr)
    {
        Nan::ThrowError("Out of memory: Unable to allocate memory resource for inquiry");
    }

    ZeroMemory(querySet, querySetSize);
    querySet->dwSize = querySetSize;
    querySet->dwNameSpace = NS_BTH;

    // Initiate client device inquiry
    HANDLE lookupServiceHandle;
    int lookupServiceError = WSALookupServiceBegin(querySet, flags, &lookupServiceHandle);
    if (lookupServiceError != SOCKET_ERROR)
    {
        // Iterate over each found bluetooth service
        bool inquiryComplete, inquiryComplete2;
        int max_rsp, num_rsp, num_rsp2;

        max_rsp = 255;
        bt_device *max_bt_device_list = (bt_device *)malloc(max_rsp * sizeof(bt_device));

        num_rsp = 0;
        inquiryComplete = false;
        while (!inquiryComplete)
        {
            flags = LUP_CONTAINERS | LUP_FLUSHCACHE | LUP_RETURN_TYPE | LUP_RES_SERVICE | LUP_RETURN_NAME | LUP_RETURN_ADDR | LUP_RETURN_BLOB;
            // For each bluetooth service retrieve its corresponding details
            lookupServiceError = WSALookupServiceNext(lookupServiceHandle, flags, &querySetSize, querySet);
            if (lookupServiceError != SOCKET_ERROR)
            {
                BTH_DEVICE_INFO *pDeviceInfo = (BTH_DEVICE_INFO *)querySet->lpBlob->pBlobData;
                char address[40] = {0};
                // Initialise LPWSTR type string to be used with WSAAddressToStringW()
                wchar_t wAddress[40];
                mbstowcs(wAddress, address, sizeof(address));
                LPWSTR lpwstrAddress = wAddress;

                DWORD addressLength = _countof(address);
                SOCKADDR_BTH *bluetoothSocketAddress = (SOCKADDR_BTH *)querySet->lpcsaBuffer->RemoteAddr.lpSockaddr;
                BTH_ADDR bluetoothAddress = bluetoothSocketAddress->btAddr;

                // Emit the corresponding event if we were able to retrieve the address
                int addressToStringError = WSAAddressToString(
                    querySet->lpcsaBuffer->RemoteAddr.lpSockaddr,
                    sizeof(SOCKADDR_BTH),
                    nullptr,
                    lpwstrAddress,
                    &addressLength);
                if (addressToStringError != SOCKET_ERROR)
                {
                    /*+++++++++++++++++\\
                    || Service Inquiry ||
                    \\+++++++++++++++++*/
                    char strippedAddress[19] = {0};
                    char name[40];
                    char name2[40];

                    // Convert address returned from WSAAddressToStringW() from LPWSTR to char *
                    wcstombs(address, lpwstrAddress, sizeof(address));

                    // Strip any leading and trailing parentheses is encountered
                    if (sscanf(address, "("
                                        "%18[^)]"
                                        ")",
                               strippedAddress) == 1)
                    {
                        strcpy(address, strippedAddress);
                    }
                    else
                    {
                        strcpy(address, address);
                    }

                    // Convert address returned from WSALookupServiceNextW() from LPWSTR to char *
                    wcstombs(name, querySet->lpszServiceInstanceName, sizeof(name));

                    HANDLE lookupServiceHandle2;
                    GUID protocol = L2CAP_PROTOCOL_UUID;

                    DWORD querySet2Size = sizeof(WSAQUERYSET);
                    WSAQUERYSET *querySet2 = (WSAQUERYSET *)malloc(querySet2Size);
                    ZeroMemory(querySet2, querySet2Size);
                    querySet2->dwSize = querySet2Size;
                    querySet2->lpServiceClassId = &protocol;
                    querySet2->dwNameSpace = NS_BTH;
                    querySet2->lpszContext = lpwstrAddress;

                    flags = LUP_FLUSHCACHE | LUP_RETURN_NAME | LUP_RETURN_TYPE | LUP_RETURN_ADDR | LUP_RETURN_BLOB | LUP_RETURN_COMMENT;
                    int lookupServiceError2 = WSALookupServiceBegin(querySet2, flags, &lookupServiceHandle2);
                    if (lookupServiceError2 != SOCKET_ERROR)
                    {
                        num_rsp2 = 0;
                        inquiryComplete2 = false;

                        int i = 1;
                        while (!inquiryComplete2)
                        {
                            BYTE buffer[2000] = {0};
                            DWORD querySet2ResultSize = sizeof(buffer);
                            WSAQUERYSET *querySet2Result = (WSAQUERYSET *)buffer;
                            ZeroMemory(querySet2Result, querySet2ResultSize);
                            querySet2Result->dwSize = querySet2ResultSize;
                            querySet2Result->lpServiceClassId = &protocol;
                            querySet2Result->dwNameSpace = NS_BTH;
                            querySet2Result->lpszContext = lpwstrAddress;
                            lookupServiceError2 = WSALookupServiceNext(lookupServiceHandle2, flags, &querySet2ResultSize, querySet2Result);
                            if (lookupServiceError2 != SOCKET_ERROR)
                            {
                                wcstombs(name2, querySet2Result->lpszServiceInstanceName, sizeof(name2));
                                // printf("%s\n", name2);
                                if (strlen(name2) == 0)
                                {
                                    continue;
                                }
                                char target[40] = "MyEars";
                                if (strcmp(name, target) == 0)
                                {
                                    BLOB *pBlob = (BLOB *)querySet2Result->lpBlob;
                                    if (!BluetoothSdpEnumAttributes(pBlob->pBlobData, pBlob->cbSize, callback, 0))
                                    {
                                        printf("BluetoothSdpEnumAttributes() failed with error code %ld\n", WSAGetLastError());
                                    }
                                    else
                                    {
                                        printf("BluetoothSdpEnumAttributes() #%d is OK!\n", i);
                                    }
                                }

                                max_bt_device_list[num_rsp].services[num_rsp2] = (char *)malloc(sizeof(name2));
                                memcpy(max_bt_device_list[num_rsp].services[num_rsp2], name2, sizeof(name2));
                                memset(name2, '\0', sizeof(name2));
                                num_rsp2++;
                            }
                            else
                            {
                                int lookupServiceErrorNumber2 = WSAGetLastError();
                                max_bt_device_list[num_rsp].servicesCount = num_rsp2;

                                if (lookupServiceErrorNumber2 == WSAEFAULT)
                                {
                                    free(querySet2);
                                    free(querySet2Result);
                                    querySet2 = (WSAQUERYSET *)malloc(querySet2Size);
                                    querySet2Result = (WSAQUERYSET *)malloc(querySet2ResultSize);
                                    if (querySet2 == nullptr)
                                    {
                                        WSALookupServiceEnd(lookupServiceHandle2);
                                        Nan::ThrowError("Out of memory: Unable to allocate memory resource for inquiry");
                                    }
                                }
                                else if (lookupServiceErrorNumber2 == WSA_E_NO_MORE)
                                {
                                    // No more services where found
                                    inquiryComplete2 = true;
                                }
                                else
                                {
                                    // Unhandled error
                                    inquiryComplete2 = true;
                                }
                            }
                            i++;
                        }
                    }
                    else
                    {
                        num_rsp2 = 0;
                        int lookupServiceErrorNumber2 = WSAGetLastError();
                        if (lookupServiceErrorNumber2 != WSASERVICE_NOT_FOUND)
                        {
                            free(querySet2);
                            Nan::ThrowError("Unable to initiate client service inquiry");
                        }
                    }

                    free(querySet2);
                    WSALookupServiceEnd(lookupServiceHandle2);

                    strcpy(max_bt_device_list[num_rsp].address, address);
                    strcpy(max_bt_device_list[num_rsp].name, name);
                    memset(name, '\0', sizeof(name));
                    num_rsp++;
                }
            }
            else
            {
                int lookupServiceErrorNumber = WSAGetLastError();
                if (lookupServiceErrorNumber == WSAEFAULT)
                {
                    free(querySet);
                    querySet = (WSAQUERYSET *)malloc(querySetSize);
                    if (querySet == nullptr)
                    {
                        WSALookupServiceEnd(lookupServiceHandle);
                        Nan::ThrowError("Out of memory: Unable to allocate memory resource for inquiry");
                    }
                }
                else if (lookupServiceErrorNumber == WSA_E_NO_MORE)
                {
                    // No more services where found
                    inquiryComplete = true;
                }
                else
                {
                    // Unhandled error
                    inquiryComplete = true;
                }
            }
        }
        inquiryResult.num_rsp = num_rsp;
        inquiryResult.devices = (bt_device *)malloc(num_rsp * sizeof(bt_device));

        for (int i = 0; i < num_rsp; i++)
        {
            int servicesCount = max_bt_device_list[i].servicesCount;
            strcpy(inquiryResult.devices[i].address, max_bt_device_list[i].address);
            strcpy(inquiryResult.devices[i].name, max_bt_device_list[i].name);
            inquiryResult.devices[i].servicesCount = max_bt_device_list[i].servicesCount;

            for (int j = 0; j < servicesCount; j++)
            {
                inquiryResult.devices[i].services[j] = (char *)malloc(40);
                memcpy(inquiryResult.devices[i].services[j], max_bt_device_list[i].services[j], 40);
            }
        }
        free(max_bt_device_list);
    }
    else
    {
        inquiryResult.num_rsp = 0;
        int lookupServiceErrorNumber = WSAGetLastError();
        if (lookupServiceErrorNumber != WSASERVICE_NOT_FOUND)
        {
            free(querySet);
            Nan::ThrowError("Unable to initiate client device inquiry");
        }
    }

    free(querySet);
    WSALookupServiceEnd(lookupServiceHandle);
    return inquiryResult;
}

DeviceINQ::DeviceINQ()
{
    initialized = BluetoothHelpers::Initialize();
}

DeviceINQ::~DeviceINQ()
{
    if (initialized)
    {
        BluetoothHelpers::Finalize();
    }
}

NAN_METHOD(DeviceINQ::New)
{
    if (info.Length() != 0)
    {
        return Nan::ThrowError("usage: DeviceINQ()");
    }

    DeviceINQ *inquire = new DeviceINQ();
    if (!inquire->Initialized)
    {
        return Nan::ThrowError("Unable to initialize socket library");
    }

    inquire->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
}

class InquireWorker : public Nan::AsyncWorker
{
public:
    InquireWorker(Nan::Callback *callback)
        : Nan::AsyncWorker(callback) {}
    ~InquireWorker() {}

    // Executed inside the worker-thread.
    // It is not safe to access V8, or V8 data structures
    // here, so everything we need for input and output
    // should go on `this`.
    void Execute()
    {
        inquiryResult = DeviceINQ::doInquire();
    }

    // Executed when the async work is complete
    // this function will be run inside the main event loop
    // so it is safe to use V8 again
    void HandleOKCallback()
    {
        Nan::HandleScope scope;

        Local<Array> resultArray = Nan::New<Array>(inquiryResult.num_rsp);

        for (int i = 0; i < inquiryResult.num_rsp; i++)
        {
            Local<Object> deviceObject = Nan::New<Object>();

            Local<String> address = Nan::New(inquiryResult.devices[i].address).ToLocalChecked();
            Local<String> name = Nan::New(inquiryResult.devices[i].name).ToLocalChecked();
            Local<Array> services = Nan::New<Array>(inquiryResult.devices[i].servicesCount);
            // Local<Array> services = Array::New(isolate);

            for (int j = 0; j < inquiryResult.devices[i].servicesCount; j++)
            {
                Local<String> service = Nan::New(inquiryResult.devices[i].services[j]).ToLocalChecked();
                Nan::Set(services, j, service);
            }

            Nan::Set(deviceObject, Nan::New("address").ToLocalChecked(), address);
            Nan::Set(deviceObject, Nan::New("name").ToLocalChecked(), name);
            Nan::Set(deviceObject, Nan::New("services").ToLocalChecked(), services);

            Nan::Set(resultArray, i, deviceObject);
        }

        Local<Value> argv[2];
        argv[0] = resultArray;
        argv[1] = resultArray;

        Nan::Call(*callback, 2, argv);
    }

private:
    bt_inquiry inquiryResult;
};

// Asynchronous access to the `Inquire()` function
NAN_METHOD(DeviceINQ::Inquire)
{
    const char *usage = "usage: inquire(callback)";
    if (info.Length() != 1)
    {
        return Nan::ThrowError(usage);
    }

    if (!info[0]->IsFunction())
    {
        return Nan::ThrowError("First argument must be a function");
    }

    Nan::Callback *callback = new Nan::Callback(info[0].As<Function>());

    Nan::AsyncQueueWorker(new InquireWorker(callback));
}

NAN_METHOD(DeviceINQ::SdpSearch)
{
    if (info.Length() != 2)
    {
        return Nan::ThrowError("usage: findSerialPortChannel(address, callback)");
    }

    if (!info[0]->IsString())
    {
        return Nan::ThrowTypeError("First argument should be a string value");
    }

    if (!info[1]->IsFunction())
    {
        return Nan::ThrowTypeError("Second argument must be a function");
    }

    sdp_baton_t *baton = new sdp_baton_t();

    Local<Function> cb = info[1].As<Function>();
    String::Utf8Value address(cb->GetIsolate(), info[0]);
    if (strcpy_s(baton->address, *address) != 0)
    {
        delete baton;
        return Nan::ThrowTypeError("Address (first argument) length is invalid");
    }

    DeviceINQ *inquire = Nan::ObjectWrap::Unwrap<DeviceINQ>(info.This());

    baton->inquire = inquire;
    baton->cb = new Nan::Callback(cb);
    baton->channelID = -1;
    baton->request.data = baton;
    baton->inquire->Ref();

    uv_queue_work(uv_default_loop(), &baton->request, EIO_SdpSearch, (uv_after_work_cb)EIO_AfterSdpSearch);

    return;
}

NAN_METHOD(DeviceINQ::ListPairedDevices)
{
    const char *usage = "usage: listPairedDevices(callback)";
    if (info.Length() != 1)
    {
        return Nan::ThrowError(usage);
    }

    if (!info[0]->IsFunction())
    {
        return Nan::ThrowTypeError("First argument must be a function");
    }
    Local<Function> cb = info[0].As<Function>();
    Local<Array> resultArray = Local<Array>(Nan::New<Array>());

    // Construct windows socket bluetooth variables
    DWORD flags = LUP_CONTAINERS | LUP_RETURN_ALL; // LUP_RETURN_NAME | LUP_RETURN_ADDR | LUP_RES_SERVICE;
    DWORD querySetSize = sizeof(WSAQUERYSET);
    WSAQUERYSET *querySet = (WSAQUERYSET *)malloc(querySetSize);
    if (querySet == nullptr)
    {
        Nan::ThrowError("Out of memory: Unable to allocate memory resource for inquiry");
    }

    ZeroMemory(querySet, querySetSize);
    querySet->dwSize = querySetSize;
    querySet->dwNameSpace = NS_BTH;

    int i = 0;

    // Initiate client device inquiry
    HANDLE lookupServiceHandle;
    int lookupServiceError = WSALookupServiceBegin(querySet, flags, &lookupServiceHandle);
    if (lookupServiceError != SOCKET_ERROR)
    {
        // Iterate over each found bluetooth service
        bool inquiryComplete = false;
        while (!inquiryComplete)
        {
            // For each bluetooth service retrieve its corresponding details
            lookupServiceError = WSALookupServiceNext(lookupServiceHandle, flags, &querySetSize, querySet);

            // only list devices that are authenticated, paired or bonded
            if (lookupServiceError != SOCKET_ERROR && (querySet->dwOutputFlags & BTHNS_RESULT_DEVICE_AUTHENTICATED))
            {
                char address[40] = {0};
                char name[40];
                wchar_t wAddress[40];
                mbstowcs(wAddress, address, sizeof(address));
                LPWSTR lpwstrAddress = wAddress;

                DWORD addressLength = _countof(address);
                SOCKADDR_BTH *bluetoothSocketAddress = (SOCKADDR_BTH *)querySet->lpcsaBuffer->RemoteAddr.lpSockaddr;
                BTH_ADDR bluetoothAddress = bluetoothSocketAddress->btAddr;

                // Emit the corresponding event if we were able to retrieve the address
                int addressToStringError = WSAAddressToString(querySet->lpcsaBuffer->RemoteAddr.lpSockaddr,
                                                              sizeof(SOCKADDR_BTH),
                                                              nullptr,
                                                              lpwstrAddress,
                                                              &addressLength);
                if (addressToStringError != SOCKET_ERROR)
                {
                    // Strip any leading and trailing parentheses is encountered
                    char strippedAddress[19] = {0};
                    wcstombs(address, lpwstrAddress, sizeof(address));
                    wcstombs(name, querySet->lpszServiceInstanceName, sizeof(name));

                    sscanf(address, "("
                                    "%18[^)]"
                                    ")",
                           strippedAddress);
                    auto addressString = sscanf(address, "("
                                                         "%18[^)]"
                                                         ")",
                                                strippedAddress) == 1
                                             ? Nan::New(strippedAddress)
                                             : Nan::New(address);

                    Local<Object> deviceObj = Nan::New<v8::Object>();
                    Nan::Set(deviceObj, Nan::New("name").ToLocalChecked(), Nan::New(name).ToLocalChecked());
                    Nan::Set(deviceObj, Nan::New("address").ToLocalChecked(), addressString.ToLocalChecked());

                    Local<Array> servicesArray = Local<Array>(Nan::New<Array>());
                    {
                        DWORD querySetSize2 = sizeof(WSAQUERYSET);
                        WSAQUERYSET *querySet2 = (WSAQUERYSET *)malloc(querySetSize2);
                        if (querySet2 == nullptr)
                        {
                            Nan::ThrowError("Out of memory: Unable to allocate memory resource for inquiry");
                        }

                        ZeroMemory(querySet2, querySetSize2);
                        querySet2->dwSize = querySetSize2;
                        querySet2->lpszContext = lpwstrAddress;
                        GUID protocol = RFCOMM_PROTOCOL_UUID; // L2CAP_PROTOCOL_UUID;
                        querySet2->lpServiceClassId = &protocol;
                        querySet2->dwNameSpace = NS_BTH;

                        DWORD flags2 = LUP_FLUSHCACHE | LUP_RETURN_NAME | LUP_RETURN_TYPE | LUP_RETURN_ADDR | LUP_RETURN_BLOB | LUP_RETURN_COMMENT;
                        HANDLE lookupServiceHandle2;

                        int place = 0;
                        int lookupServiceError2 = WSALookupServiceBegin(querySet2, flags2, &lookupServiceHandle2);
                        if (lookupServiceError2 != SOCKET_ERROR)
                        {
                            bool inquiryComplete2 = false;
                            while (!inquiryComplete2)
                            {
                                lookupServiceError2 = WSALookupServiceNext(lookupServiceHandle2, flags2, &querySetSize2, querySet2);
                                if (lookupServiceError2 != SOCKET_ERROR)
                                {
                                    int port = (int)((SOCKADDR_BTH *)querySet2->lpcsaBuffer->RemoteAddr.lpSockaddr)->port;
                                    Local<Object> serviceObj = Nan::New<v8::Object>();
                                    Nan::Set(serviceObj, Nan::New("channel").ToLocalChecked(), Nan::New(port));
                                    Nan::Set(serviceObj, Nan::New("name").ToLocalChecked(), Nan::New(name).ToLocalChecked());
                                    Nan::Set(servicesArray, place++, serviceObj);
                                }
                                else
                                {
                                    int lookupServiceErrorNumber = WSAGetLastError();
                                    if (lookupServiceErrorNumber == WSAEFAULT)
                                    {
                                        free(querySet2);
                                        querySet2 = (WSAQUERYSET *)malloc(querySetSize2);
                                        if (querySet2 == nullptr)
                                        {
                                            WSALookupServiceEnd(lookupServiceHandle2);
                                            Nan::ThrowError("Out of memory: Unable to allocate memory resource for inquiry");
                                        }
                                    }
                                    else if (lookupServiceErrorNumber == WSA_E_NO_MORE)
                                    {
                                        // No more services where found
                                        inquiryComplete2 = true;
                                    }
                                    else
                                    {
                                        // Unhandled error
                                        inquiryComplete2 = true;
                                    }
                                }
                            }
                        }
                        else
                        {
                            int lookupServiceErrorNumber = WSAGetLastError();
                            if (lookupServiceErrorNumber != WSASERVICE_NOT_FOUND)
                            {
                                free(querySet);
                                Nan::ThrowError("Unable to initiate client device inquiry");
                            }
                        }
                        free(querySet2);
                    }

                    Nan::Set(deviceObj, Nan::New("services").ToLocalChecked(), servicesArray);
                    Nan::Set(resultArray, i, deviceObj);
                    i = i + 1;
                }
            }
            else
            {
                int lookupServiceErrorNumber = WSAGetLastError();
                if (lookupServiceErrorNumber == WSAEFAULT)
                {
                    free(querySet);
                    querySet = (WSAQUERYSET *)malloc(querySetSize);
                    if (querySet == nullptr)
                    {
                        WSALookupServiceEnd(lookupServiceHandle);
                        Nan::ThrowError("Out of memory: Unable to allocate memory resource for inquiry");
                    }
                }
                else if (lookupServiceErrorNumber == WSA_E_NO_MORE)
                {
                    // No more services where found
                    inquiryComplete = true;
                }
                else
                {
                    // Unhandled error
                    inquiryComplete = true;
                }
            }
        }
    }
    else
    {
        int lookupServiceErrorNumber = WSAGetLastError();
        if (lookupServiceErrorNumber != WSASERVICE_NOT_FOUND)
        {
            free(querySet);
            Nan::ThrowError("Unable to initiate client device inquiry");
        }
    }

    free(querySet);
    WSALookupServiceEnd(lookupServiceHandle);

    // build an array of objects representing a paired device:
    //  ex: {
    //    name: 'MyBluetoothDeviceName',
    //    address: '12-34-56-78-90',
    //    services: [
    //      { name: 'SPP', channel: 1 },
    //      { name: 'iAP', channel: 2 }
    //    ]
    //  }
    Local<Value> argv[1] = {
        resultArray};
    cb->Call(Nan::GetCurrentContext(), Nan::GetCurrentContext()->Global(), 1, argv);

    return;
}
