#include <sl.h>
#include <sld3dkmt.h>
#include <string.h>
#include <push.h>


GUID GUID_DISPLAY_DEVICE_ARRIVAL_I  = { 0x1ca05180, 0xa699, 0x450a, { 0x9a, 0x0c, 0xde, 0x4f, 0xbe, 0x3d, 0xdd, 0x89 } };


typedef struct _PH_UINT64_DELTA
{
    UINT64 Value;
    UINT64 Delta;
} PH_UINT64_DELTA, *PPH_UINT64_DELTA;


TYPE_D3DKMTOpenAdapterFromDeviceName    D3DKMTOpenAdapterFromDeviceName = NULL;
TYPE_D3DKMTQueryStatistics              D3DKMTQueryStatistics           = NULL;
PPH_UINT64_DELTA EtGpuNodesTotalRunningTimeDelta;
struct _PH_STRING;
typedef struct _PH_STRING *PPH_STRING;
typedef struct _ETP_GPU_ADAPTER
{
    LUID AdapterLuid;
    PPH_STRING Description;
    ULONG SegmentCount;
    ULONG NodeCount;
    ULONG FirstNodeIndex;

    RTL_BITMAP ApertureBitMap;
    ULONG ApertureBitMapBuffer[1];
} ETP_GPU_ADAPTER, *PETP_GPU_ADAPTER;


VOID InitializeD3DStatistics();
VOID UpdateNodeInformation();
PETP_GPU_ADAPTER AllocateGpuAdapter(
    UINT32 NumberOfSegments
    );


UINT32 EtGpuTotalNodeCount;
PETP_GPU_ADAPTER D3dkmt_GpuAdapter;
UINT32 EtGpuTotalSegmentCount;
UINT32 EtGpuNextNodeIndex = 0;
UINT32 *EtGpuNodeBitMapBuffer;


#define BYTES_NEEDED_FOR_BITS(Bits) ((((Bits) + sizeof(UINT32) * 8 - 1) / 8) & ~(UINT32)(sizeof(UINT32) - 1)) // divide round up
#define DIGCF_PRESENT           0x00000002
#define DIGCF_DEVICEINTERFACE   0x00000010
#define FIELD_OFFSET(type, field)    ((INT32)(INT32)&(((type *)0)->field))

#define PhUpdateDelta(DltMgr, NewValue) \
    ((DltMgr)->Delta = (NewValue) - (DltMgr)->Value, \
    (DltMgr)->Value = (NewValue), (DltMgr)->Delta)


extern "C"
{
VOID __stdcall RtlInitializeBitMap(
  RTL_BITMAP* BitMapHeader,
  UINT32 *BitMapBuffer,
  UINT32 SizeOfBitMap
);
VOID
__stdcall
RtlSetBits(
    RTL_BITMAP* BitMapHeader,
    ULONG StartingIndex,
    ULONG NumberToSet
    );
}

RTL_BITMAP EtGpuNodeBitMap;


LARGE_INTEGER EtClockTotalRunningTimeFrequency;
PH_UINT64_DELTA EtClockTotalRunningTimeDelta;
PH_UINT64_DELTA EtGpuTotalRunningTimeDelta;
PH_UINT64_DELTA EtGpuSystemRunningTimeDelta;

FLOAT EtGpuNodeUsage;
UINT64 EtGpuDedicatedLimit;


BOOLEAN
RtlCheckBit(
    RTL_BITMAP* BitMapHeader,
    ULONG BitPosition
    )
{
    return (((LONG*)BitMapHeader->Buffer)[BitPosition / 32] >> (BitPosition % 32)) & 0x1;
}


PETP_GPU_ADAPTER AllocateGpuAdapter( UINT32 NumberOfSegments )
{
    PETP_GPU_ADAPTER adapter;
    UINT32 sizeNeeded;

    sizeNeeded = FIELD_OFFSET(ETP_GPU_ADAPTER, ApertureBitMapBuffer);
    sizeNeeded += BYTES_NEEDED_FOR_BITS(NumberOfSegments);

    adapter = (PETP_GPU_ADAPTER) Memory::Allocate(sizeNeeded);

    Memory::Clear(adapter, sizeNeeded);

    return adapter;
}


UINT8 D3DKMT_GetGpuUsage()
{
    double elapsedTime; // total GPU node elapsed time in micro-seconds
    INT32 usage = 0;


    UpdateNodeInformation();

    elapsedTime = (double)EtClockTotalRunningTimeDelta.Delta * 10000000 / EtClockTotalRunningTimeFrequency.QuadPart;

    if (elapsedTime != 0)
    {
        EtGpuNodeUsage = (float) (EtGpuTotalRunningTimeDelta.Delta / elapsedTime);
        usage = EtGpuNodeUsage * 100;
    }
    else
        EtGpuNodeUsage = 0;

    if (EtGpuNodeUsage > 1)
        EtGpuNodeUsage = 1;

     // Clip calculated usage to [0-100] range to filter calculation non-ideality.

     if (usage < 0)
         usage = 0;

     if (usage > 100)
         usage = 100;

    return usage;
}


UINT64
D3DKMTGetMemoryUsage()
{
    ULONG i;
    D3DKMT_QUERYSTATISTICS queryStatistics;
    UINT64 dedicatedUsage;

    dedicatedUsage = 0;

    for (i = 0; i < D3dkmt_GpuAdapter->SegmentCount; i++)
    {
        memset(&queryStatistics, 0, sizeof(D3DKMT_QUERYSTATISTICS));

        queryStatistics.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
        queryStatistics.AdapterLuid = D3dkmt_GpuAdapter->AdapterLuid;
        queryStatistics.QuerySegment.SegmentId = i;

        if (NT_SUCCESS(D3DKMTQueryStatistics(&queryStatistics)))
        {
            UINT64 bytesCommitted;

            bytesCommitted = queryStatistics.QueryResult.SegmentInformationV1.BytesCommitted;

            if (!RtlCheckBit(&D3dkmt_GpuAdapter->ApertureBitMap, i))
                dedicatedUsage += bytesCommitted;
        }
    }

    return dedicatedUsage;
}

D3DKMT_OPENADAPTERFROMDEVICENAME    openAdapterFromDeviceName;

typedef INT32(__stdcall *TYPE_SetupDiDestroyDeviceInfoList)( VOID *DeviceInfoSet );

typedef VOID*(__stdcall *TYPE_SetupDiGetClassDevsW)( 
    const GUID* ClassGuid, 
    const WCHAR* Enumerator, 
    VOID* hwndParent,
    DWORD Flags
    );

typedef INT32(__stdcall *TYPE_SetupDiEnumDeviceInterfaces)(
    VOID                        *DeviceInfoSet,
    SP_DEVINFO_DATA             *DeviceInfoData,
    const GUID                  *InterfaceClassGuid,
    DWORD                       MemberIndex,
    SP_DEVICE_INTERFACE_DATA    *DeviceInterfaceData
    );

typedef INT32(__stdcall *TYPE_SetupDiGetDeviceInterfaceDetailW)(
    VOID                                *DeviceInfoSet,
    SP_DEVICE_INTERFACE_DATA            *DeviceInterfaceData,
    SP_DEVICE_INTERFACE_DETAIL_DATA_W   *DeviceInterfaceDetailData,
    DWORD                               DeviceInterfaceDetailDataSize,
    DWORD                               *RequiredSize,
    SP_DEVINFO_DATA                     *DeviceInfoData
    );

TYPE_SetupDiDestroyDeviceInfoList       SetupDiDestroyDeviceInfoList;
TYPE_SetupDiGetClassDevsW               SetupDiGetClassDevsW;
TYPE_SetupDiEnumDeviceInterfaces        SetupDiEnumDeviceInterfaces;
TYPE_SetupDiGetDeviceInterfaceDetailW   SetupDiGetDeviceInterfaceDetailW;


VOID D3DKMTInitialize()
{
    HANDLE gdi32 = NULL;
    HANDLE setupapi = NULL;
    VOID *deviceInfoSet;
    UINT32 result;
    UINT32 memberIndex;
    UINT32 detailDataSize;
    SP_DEVICE_INTERFACE_DATA            deviceInterfaceData;
    SP_DEVICE_INTERFACE_DETAIL_DATA_W   *detailData;
    SP_DEVINFO_DATA                     deviceInfoData;
    //D3DKMT_OPENADAPTERFROMDEVICENAME    openAdapterFromDeviceName;
    D3DKMT_QUERYSTATISTICS              queryStatistics;

    gdi32 = Module::Load(L"gdi32.dll");

    if (!gdi32)
    {
        return;
    }

    D3DKMTOpenAdapterFromDeviceName = (TYPE_D3DKMTOpenAdapterFromDeviceName) Module::GetProcedureAddress(
                                                                                gdi32,
                                                                                "D3DKMTOpenAdapterFromDeviceName"
                                                                                );

    D3DKMTQueryStatistics = (TYPE_D3DKMTQueryStatistics) Module::GetProcedureAddress(gdi32, "D3DKMTQueryStatistics");

    if (!D3DKMTOpenAdapterFromDeviceName || !D3DKMTQueryStatistics)
    {
        return;
    }

    setupapi = Module::Load(L"setupapi.dll");

    SetupDiGetClassDevsW = (TYPE_SetupDiGetClassDevsW) 
        Module::GetProcedureAddress(setupapi, "SetupDiGetClassDevsW");

    SetupDiDestroyDeviceInfoList = (TYPE_SetupDiDestroyDeviceInfoList) 
        Module::GetProcedureAddress(setupapi, "SetupDiDestroyDeviceInfoList");

    SetupDiEnumDeviceInterfaces = (TYPE_SetupDiEnumDeviceInterfaces) 
        Module::GetProcedureAddress(setupapi, "SetupDiEnumDeviceInterfaces");

    SetupDiGetDeviceInterfaceDetailW = (TYPE_SetupDiGetDeviceInterfaceDetailW)
        Module::GetProcedureAddress(setupapi, "SetupDiGetDeviceInterfaceDetailW");

    deviceInfoSet = SetupDiGetClassDevsW(&GUID_DISPLAY_DEVICE_ARRIVAL_I, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

    if (!deviceInfoSet)
    {
        return;
    }

    memberIndex = 0;
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    while (SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &GUID_DISPLAY_DEVICE_ARRIVAL_I, memberIndex, &deviceInterfaceData))
    {
        detailDataSize = 0x100;
        detailData = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*) Memory::Allocate(detailDataSize);
        detailData->cbSize = 6; /*sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)*/
        deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

        result = SetupDiGetDeviceInterfaceDetailW(
                    deviceInfoSet,
                    &deviceInterfaceData,
                    detailData,
                    detailDataSize,
                    &detailDataSize,
                    &deviceInfoData
                    );

        if (result)
        {
            openAdapterFromDeviceName.pDeviceName = detailData->DevicePath;

            if (NT_SUCCESS(D3DKMTOpenAdapterFromDeviceName(&openAdapterFromDeviceName)))
            {
                memset(&queryStatistics, 0, sizeof(D3DKMT_QUERYSTATISTICS));

                queryStatistics.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
                queryStatistics.AdapterLuid = openAdapterFromDeviceName.AdapterLuid;

                if (NT_SUCCESS(D3DKMTQueryStatistics(&queryStatistics)))
                {
                    UINT32 i;

                    D3dkmt_GpuAdapter = AllocateGpuAdapter(queryStatistics.QueryResult.AdapterInformation.NbSegments);

                    D3dkmt_GpuAdapter->AdapterLuid       = openAdapterFromDeviceName.AdapterLuid;
                    D3dkmt_GpuAdapter->NodeCount     = queryStatistics.QueryResult.AdapterInformation.NodeCount;
                    D3dkmt_GpuAdapter->SegmentCount  = queryStatistics.QueryResult.AdapterInformation.NbSegments;

                    RtlInitializeBitMap(
                        &D3dkmt_GpuAdapter->ApertureBitMap,
                        D3dkmt_GpuAdapter->ApertureBitMapBuffer,
                        queryStatistics.QueryResult.AdapterInformation.NbSegments
                        );

                    EtGpuTotalNodeCount += D3dkmt_GpuAdapter->NodeCount;

                    EtGpuTotalSegmentCount += D3dkmt_GpuAdapter->SegmentCount;

                    D3dkmt_GpuAdapter->FirstNodeIndex = EtGpuNextNodeIndex;
                    EtGpuNextNodeIndex += D3dkmt_GpuAdapter->NodeCount;

                    for (i = 0; i < D3dkmt_GpuAdapter->SegmentCount; i++)
                    {
                        memset(&queryStatistics, 0, sizeof(D3DKMT_QUERYSTATISTICS));

                        queryStatistics.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
                        queryStatistics.AdapterLuid = D3dkmt_GpuAdapter->AdapterLuid;
                        queryStatistics.QuerySegment.SegmentId = i;

                        if (NT_SUCCESS(D3DKMTQueryStatistics(&queryStatistics)))
                        {
                            UINT64 commitLimit;
                            UINT32 aperature;

                            commitLimit = queryStatistics.QueryResult.SegmentInformationV1.CommitLimit;
                            aperature = queryStatistics.QueryResult.SegmentInformationV1.Aperture;

                            if (aperature)
                                RtlSetBits(&D3dkmt_GpuAdapter->ApertureBitMap, i, 1);
                            else
                                EtGpuDedicatedLimit += commitLimit;
                        }
                    }
                }
            }
        }

        Memory::Free(detailData);

        memberIndex++;
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);

    EtGpuNodeBitMapBuffer = (UINT32*) Memory::Allocate(BYTES_NEEDED_FOR_BITS(EtGpuTotalNodeCount));
    
    RtlInitializeBitMap(&EtGpuNodeBitMap, EtGpuNodeBitMapBuffer, EtGpuTotalNodeCount);

    EtGpuNodesTotalRunningTimeDelta = (PPH_UINT64_DELTA) Memory::Allocate(sizeof(PH_UINT64_DELTA) * EtGpuTotalNodeCount);

    memset(EtGpuNodesTotalRunningTimeDelta, 0, sizeof(PH_UINT64_DELTA) * EtGpuTotalNodeCount);
}


VOID
UpdateNodeInformation()
{
    UINT32 j;
    D3DKMT_QUERYSTATISTICS queryStatistics;
    UINT64 totalRunningTime;
    UINT64 systemRunningTime;
    LARGE_INTEGER performanceCounter;
    static BOOLEAN initialized = FALSE;

    //check if initialized
    if (!initialized)
    {
        D3DKMTInitialize();
        initialized = TRUE;
    }

    totalRunningTime = 0;
    systemRunningTime = 0;

    if (D3dkmt_GpuAdapter == NULL)
    {
        return;
    }

    for (j = 0; j < D3dkmt_GpuAdapter->NodeCount; j++)
    {
        memset(&queryStatistics, 0, sizeof(D3DKMT_QUERYSTATISTICS));

        queryStatistics.Type                = D3DKMT_QUERYSTATISTICS_NODE;
        queryStatistics.AdapterLuid         = D3dkmt_GpuAdapter->AdapterLuid;
        queryStatistics.QueryNode.NodeId    = j;

        if (NT_SUCCESS(D3DKMTQueryStatistics(&queryStatistics)))
        {
            UINT32 nodeIndex;

            nodeIndex = D3dkmt_GpuAdapter->FirstNodeIndex + j;

            PhUpdateDelta(
                &EtGpuNodesTotalRunningTimeDelta[nodeIndex],
                queryStatistics.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart
                );

            totalRunningTime += queryStatistics.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart;
            systemRunningTime += queryStatistics.QueryResult.NodeInformation.SystemInformation.RunningTime.QuadPart;
        }
    }

    NtQueryPerformanceCounter(&performanceCounter, &EtClockTotalRunningTimeFrequency);

    PhUpdateDelta(&EtClockTotalRunningTimeDelta, performanceCounter.QuadPart);
    PhUpdateDelta(&EtGpuTotalRunningTimeDelta, totalRunningTime);
    PhUpdateDelta(&EtGpuSystemRunningTimeDelta, systemRunningTime);
}


typedef enum _D3DKMT_ESCAPETYPE
{
    D3DKMT_ESCAPE_DRIVERPRIVATE = 0,
    D3DKMT_ESCAPE_VIDMM = 1,
    D3DKMT_ESCAPE_TDRDBGCTRL = 2,
    D3DKMT_ESCAPE_VIDSCH = 3,
    D3DKMT_ESCAPE_DEVICE = 4,
    D3DKMT_ESCAPE_DMM = 5,
    D3DKMT_ESCAPE_DEBUG_SNAPSHOT = 6,
    D3DKMT_ESCAPE_SETDRIVERUPDATESTATUS = 7,
    D3DKMT_ESCAPE_DRT_TEST = 8,
    D3DKMT_ESCAPE_DIAGNOSTICS = 9
} D3DKMT_ESCAPETYPE;

typedef struct _D3DDDI_ESCAPEFLAGS
{
    union
    {
        struct
        {
            UINT32    HardwareAccess : 1;    // 0x00000001
            UINT32    Reserved : 31;    // 0xFFFFFFFE
        };
        UINT32        Value;
    };
} D3DDDI_ESCAPEFLAGS;

typedef struct _D3DKMT_ESCAPE
{
    D3DKMT_HANDLE       hAdapter;               // in: adapter handle
    D3DKMT_HANDLE       hDevice;                // in: device handle [Optional]
    D3DKMT_ESCAPETYPE   Type;                   // in: escape type.
    D3DDDI_ESCAPEFLAGS  Flags;                  // in: flags
    VOID*               pPrivateDriverData;     // in/out: escape data
    UINT32                PrivateDriverDataSize;  // in: size of escape data
    D3DKMT_HANDLE       hContext;               // in: context handle [Optional]
} D3DKMT_ESCAPE;

typedef NTSTATUS (__stdcall *TYPE_D3DKMTEscape)(
    const D3DKMT_ESCAPE *pData
    );

TYPE_D3DKMTEscape D3DKMTEscape;

VOID D3DKMT_GetPrivateDriverData( VOID* PrivateDriverData, UINT32 PrivateDriverDataSize )
{
    D3DKMT_ESCAPE driverInformation = { 0 };

    driverInformation.hAdapter = openAdapterFromDeviceName.hAdapter;
    driverInformation.hDevice = NULL;
    driverInformation.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    driverInformation.Flags.Value = 0;
    driverInformation.pPrivateDriverData = PrivateDriverData;
    driverInformation.PrivateDriverDataSize = PrivateDriverDataSize;
    driverInformation.hContext = NULL;

    D3DKMTEscape = (TYPE_D3DKMTEscape) Module::GetProcedureAddress(Module::Load(L"gdi32.dll"), "D3DKMTEscape");
    D3DKMTEscape(&driverInformation);
}


UINT16 D3DKMT_GetEngineClock(){ return 0; };
UINT16 D3DKMT_GetMemoryClock(){ return 0; };
UINT16 D3DKMT_GetMaxEngineClock(){ return 0; };
UINT16 D3DKMT_GetMaxMemoryClock(){ return 0; };
UINT64 D3DKMT_GetTotalMemory(){ return 0; };
UINT64 D3DKMT_GetFreeMemory(){ return 0; };
UINT8  D3DKMT_GetTemperature(){ return 0; };
VOID   D3DKMT_ForceMaximumClocks(){};