/**
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
#ifndef OPENDNP3_SINGLERESPONSETASK_H
#define OPENDNP3_SINGLERESPONSETASK_H

#include "opendnp3/master/IMasterTask.h"

namespace opendnp3
{

/**
* Base class for tasks that only require a single response
*/
class SingleResponseTask : public IMasterTask
{	

public:	

	SingleResponseTask(openpal::Logger logger);

	SingleResponseTask(bool enabled, openpal::Logger logger);

	SingleResponseTask(bool enabled, openpal::MonotonicTimestamp expiration, openpal::Logger logger);
	
	virtual Result OnResponse(const APDUResponseHeader& response, const openpal::ReadOnlyBuffer& objects, const openpal::MonotonicTimestamp& now) override final;	

protected:
	
	virtual void OnBadControlOctet(const openpal::MonotonicTimestamp& now) = 0;

	virtual Result OnOnlyResponse(const APDUResponseHeader& response, const openpal::ReadOnlyBuffer& objects, const openpal::MonotonicTimestamp& now) = 0;

	openpal::Logger logger;	
};

} //end ns


#endif
