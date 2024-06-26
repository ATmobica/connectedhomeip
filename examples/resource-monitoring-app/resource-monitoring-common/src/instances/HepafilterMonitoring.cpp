/*
 *
 *    Copyright (c) 2023 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <StaticReplacementProductListManager.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/clusters/resource-monitoring-server/resource-monitoring-cluster-objects.h>
#include <app/data-model/Nullable.h>
#include <app/util/config.h>
#include <app/util/endpoint-config-api.h>
#include <instances/HepaFilterMonitoring.h>
#include <lib/core/CHIPError.h>
#include <lib/support/CodeUtils.h>
#include <protocols/interaction_model/StatusCode.h>

using namespace chip;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::ResourceMonitoring;
using chip::Protocols::InteractionModel::Status;

static ResourceMonitoring::Attributes::ReplacementProductStruct::Type sHepaFilterReplacementProductsList[] = {
    { .productIdentifierType  = ResourceMonitoring::ProductIdentifierTypeEnum::kUpc,
      .productIdentifierValue = CharSpan::fromCharString("111112222233") },
    { .productIdentifierType  = ResourceMonitoring::ProductIdentifierTypeEnum::kGtin8,
      .productIdentifierValue = CharSpan::fromCharString("gtin8xhe") },
    { .productIdentifierType  = ResourceMonitoring::ProductIdentifierTypeEnum::kEan,
      .productIdentifierValue = CharSpan::fromCharString("4444455555666") },
    { .productIdentifierType  = ResourceMonitoring::ProductIdentifierTypeEnum::kGtin14,
      .productIdentifierValue = CharSpan::fromCharString("gtin14xhepaxxx") },
    { .productIdentifierType  = ResourceMonitoring::ProductIdentifierTypeEnum::kOem,
      .productIdentifierValue = CharSpan::fromCharString("oem20xhepaxxxxxxxxxx") },
};
StaticReplacementProductListManager sHepaFilterReplacementProductListManager(&sHepaFilterReplacementProductsList[0],
                                                                             ArraySize(sHepaFilterReplacementProductsList));

//-- Hepa filter Monitoring instance methods
CHIP_ERROR HepaFilterMonitoringInstance::AppInit()
{
    ChipLogDetail(Zcl, "HepaFilterMonitoringInstance::Init()");

    SetReplacementProductListManagerInstance(&sHepaFilterReplacementProductListManager);

    return CHIP_NO_ERROR;
}

Status HepaFilterMonitoringInstance::PreResetCondition()
{
    ChipLogDetail(Zcl, "HepaFilterMonitoringInstance::PreResetCondition()");
    return Status::Success;
}

Status HepaFilterMonitoringInstance::PostResetCondition()
{
    ChipLogDetail(Zcl, "HepaFilterMonitoringInstance::PostResetCondition()");
    return Status::Success;
}
