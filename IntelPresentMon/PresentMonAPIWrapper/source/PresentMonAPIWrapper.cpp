#include "../../PresentMonAPI2/source/PresentMonAPI.h"
#include "PresentMonAPIWrapper.h"

namespace pmapi::intro
{
    EnumKeyView MetricView::GetMetricKey() const
    {
        return pDataset->FindEnumKey(PM_ENUM_METRIC, (int)pBase->id);
    }

    EnumKeyView MetricView::GetUnit() const
    {
        return pDataset->FindEnumKey(PM_ENUM_UNIT, (int)pBase->unit);
    }

    EnumKeyView DataTypeInfoView::GetType() const
    {
        return pDataset->FindEnumKey(PM_ENUM_DATA_TYPE, (int)pBase->type);
    }

    EnumView DataTypeInfoView::GetEnum() const
    {
        // TODO: throw exception if datatype is not an enum
        return pDataset->FindEnum(pBase->enumId);
    }

    EnumKeyView DeviceView::GetType() const
    {
        return pDataset->FindEnumKey(PM_ENUM_DEVICE_TYPE, (int)pBase->type);
    }

    EnumKeyView DeviceView::GetVendor() const
    {
        return pDataset->FindEnumKey(PM_ENUM_DEVICE_VENDOR, (int)pBase->vendor);
    }

    DeviceView DeviceMetricInfoView::GetDevice() const
    {
        return pDataset->FindDevice(pBase->deviceId);
    }

    EnumKeyView DeviceMetricInfoView::GetAvailablity() const
    {
        return pDataset->FindEnumKey(PM_ENUM_METRIC_AVAILABILITY, (int)pBase->availability);
    }
}
