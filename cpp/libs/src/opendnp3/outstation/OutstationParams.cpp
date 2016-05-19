/*
 * Licensed to Green Energy Corp (www.greenenergycorp.com) under one or
 * more contributor license agreements. See the NOTICE file distributed
 * with this work for additional information regarding copyright ownership.
 * Green Energy Corp licenses this file to you under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This project was forked on 01/01/2013 by Automatak, LLC and modifications
 * may have been made to this file. Automatak, LLC licenses these modifications
 * to you under the terms of the License.
 */
#include "opendnp3/outstation/OutstationParams.h"

#include "opendnp3/app/AppConstants.h"

using namespace openpal;

namespace opendnp3
{

OutstationParams::OutstationParams() :
	indexMode(IndexMode::Contiguous),
	maxControlsPerRequest(16),
	selectTimeout(TimeDuration::Seconds(10)),
	solConfirmTimeout(DEFAULT_APP_TIMEOUT),
	unsolConfirmTimeout(DEFAULT_APP_TIMEOUT),
	unsolRetryTimeout(DEFAULT_APP_TIMEOUT),
	maxTxFragSize(DEFAULT_MAX_APDU_SIZE),
	maxRxFragSize(DEFAULT_MAX_APDU_SIZE),
	allowUnsolicited(false),
	ignoreRepeatReads(true),
	typesAllowedInClass0(StaticTypeBitField::AllTypes())
{

}

}

