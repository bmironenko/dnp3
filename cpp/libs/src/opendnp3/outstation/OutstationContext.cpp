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
#include "OutstationContext.h"

#include "opendnp3/LogLevels.h"

#include "opendnp3/app/parsing/APDUParser.h"
#include "opendnp3/app/parsing/APDUHeaderParser.h"

#include "opendnp3/app/Functions.h"
#include "opendnp3/app/APDULogging.h"
#include "opendnp3/app/APDUBuilders.h"

#include "opendnp3/outstation/ReadHandler.h"
#include "opendnp3/outstation/WriteHandler.h"
#include "opendnp3/outstation/IINHelpers.h"
#include "opendnp3/outstation/CommandActionAdapter.h"
#include "opendnp3/outstation/CommandResponseHandler.h"
#include "opendnp3/outstation/ConstantCommandAction.h"
#include "opendnp3/outstation/EventWriter.h"

#include "opendnp3/outstation/ClassBasedRequestHandler.h"
#include "opendnp3/outstation/AssignClassHandler.h"

#include <openpal/logging/LogMacros.h>

using namespace openpal;

namespace opendnp3
{

OContext::OContext(
    const OutstationConfig& config,
    const DatabaseTemplate& dbTemplate,
    openpal::Logger logger_,
    openpal::IExecutor& executor,
    ILowerLayer& lower,
    ICommandHandler& commandHandler,
    IOutstationApplication& application) :

	logger(logger_),
	pExecutor(&executor),
	pLower(&lower),
	pCommandHandler(&commandHandler),
	pApplication(&application),
	eventBuffer(config.eventBufferConfig),
	database(dbTemplate, eventBuffer, config.params.indexMode, config.params.typesAllowedInClass0),
	rspContext(database.GetResponseLoader(), eventBuffer),
	params(config.params),
	isOnline(false),
	isTransmitting(false),
	staticIIN(IINBit::DEVICE_RESTART),
	confirmTimer(executor),
	deferred(config.params.maxRxFragSize),
	sol(config.params.maxTxFragSize),
	unsol(config.params.maxTxFragSize)
{

}

bool OContext::OnLowerLayerUp()
{
	if (isOnline)
	{
		SIMPLE_LOG_BLOCK(logger, flags::ERR, "already online");
		return false;
	}

	isOnline = true;
	this->CheckForTaskStart();
	return true;
}

bool OContext::OnLowerLayerDown()
{
	if (!isOnline)
	{
		SIMPLE_LOG_BLOCK(logger, flags::ERR, "already offline");
		return false;
	}

	isOnline = false;
	isTransmitting = false;

	sol.Reset();
	unsol.Reset();
	history.Reset();
	deferred.Reset();
	eventBuffer.Unselect();
	rspContext.Reset();
	confirmTimer.Cancel();

	return true;
}

bool OContext::OnSendResult(bool isSuccess)
{
	if (!isOnline || !isTransmitting)
	{
		return false;
	}

	this->isTransmitting = false;
	this->CheckForTaskStart();
	return true;
}

bool OContext::OnReceive(const openpal::RSlice& fragment)
{
	if (!this->isOnline)
	{
		SIMPLE_LOG_BLOCK(this->logger, flags::ERR, "ignoring received data while offline");
		return false;
	}


	this->Increment(SecurityStatIndex::TOTAL_MESSAGES_RX);
	this->ParseHeader(fragment);
	this->CheckForTaskStart();
	return true;
}

OutstationSolicitedStateBase* OContext::OnReceiveSolRequest(const APDUHeader& header, const openpal::RSlice& objects)
{
	// analyze this request to see how it compares to the last request
	if (this->history.HasLastRequest())
	{
		if (this->sol.seq.num.Equals(header.control.SEQ))
		{
			if (this->history.FullyEqualsLastRequest(header, objects))
			{
				if (header.function == FunctionCode::READ)
				{
					if (params.ignoreRepeatReads)
					{
						SIMPLE_LOG_BLOCK(this->logger, flags::WARN, "Ignoring repeat read request");
						return this->sol.pState;
					}
					else
					{
						SIMPLE_LOG_BLOCK(this->logger, flags::DBG, "Repeat read request");
						return this->ProcessNewRequest(header, objects);
					}
				}
				else
				{
					return this->sol.pState->OnRepeatNonReadRequest(*this, header, objects);
				}
			}
			else // new operation with same SEQ
			{
				return this->ProcessNewRequest(header, objects);
			}
		}
		else  // completely new sequence #
		{
			return this->ProcessNewRequest(header, objects);
		}
	}
	else
	{
		return this->ProcessNewRequest(header, objects);
	}
}

OutstationSolicitedStateBase* OContext::ProcessNewRequest(const APDUHeader& header, const openpal::RSlice& objects)
{
	this->sol.seq.num = header.control.SEQ;

	if (header.function == FunctionCode::READ)
	{
		return this->sol.pState->OnNewReadRequest(*this, header, objects);
	}
	else
	{
		return this->sol.pState->OnNewNonReadRequest(*this, header, objects);
	}
}

void OContext::ReceiveParsedHeader(const openpal::RSlice& apdu, const APDUHeader& header, const openpal::RSlice& objects)
{
	// this look strange, but this method is overridable for SA
	this->ProcessAPDU(apdu, header, objects);
}

void OContext::ProcessAPDU(const openpal::RSlice& apdu, const APDUHeader& header, const openpal::RSlice& objects)
{
	if (Functions::IsNoAckFuncCode(header.function))
	{
		// this is the only request we process while we are transmitting
		// because it doesn't require a response of any kind
		this->ProcessRequestNoAck(header, objects);
	}
	else
	{
		if (this->isTransmitting)
		{
			this->deferred.Set(header, objects);
		}
		else
		{
			if (header.function == FunctionCode::CONFIRM)
			{
				this->ProcessConfirm(header);
			}
			else
			{
				this->ProcessRequest(header, objects);
			}
		}
	}
}


void OContext::ProcessRequest(const APDUHeader& header, const openpal::RSlice& objects)
{
	if (header.control.UNS)
	{
		FORMAT_LOG_BLOCK(this->logger, flags::WARN, "Ignoring unsol with invalid function code: %s", FunctionCodeToString(header.function));
	}
	else
	{
		this->sol.pState = this->OnReceiveSolRequest(header, objects);
	}
}

void OContext::ProcessConfirm(const APDUHeader& header)
{
	if (header.control.UNS)
	{
		this->unsol.pState = this->unsol.pState->OnConfirm(*this, header);
	}
	else
	{
		this->sol.pState = this->sol.pState->OnConfirm(*this, header);
	}
}

void OContext::BeginResponseTx(const RSlice& response)
{
	this->sol.tx.Record(response);
	this->BeginTx(response);
}

void OContext::BeginUnsolTx(const RSlice& response)
{
	this->unsol.tx.Record(response);
	this->unsol.seq.confirmNum = this->unsol.seq.num;
	this->unsol.seq.num.Increment();
	this->BeginTx(response);
}

void OContext::BeginTx(const openpal::RSlice& response)
{
	logging::ParseAndLogResponseTx(this->logger, response);
	this->isTransmitting = true;
	this->pLower->BeginTransmit(response);
	this->Increment(SecurityStatIndex::TOTAL_MESSAGES_TX);
}

void OContext::CheckForDeferredRequest()
{
	if (this->CanTransmit() && this->deferred.IsSet())
	{
		auto handler = [this](const APDUHeader & header, const RSlice & objects)
		{
			return this->ProcessDeferredRequest(header, objects);
		};
		this->deferred.Process(handler);
	}
}

bool OContext::ProcessDeferredRequest(APDUHeader header, openpal::RSlice objects)
{
	if (header.function == FunctionCode::CONFIRM)
	{
		this->ProcessConfirm(header);
		return true;
	}
	else
	{
		if (header.function == FunctionCode::READ)
		{
			if (this->unsol.IsIdle())
			{
				this->ProcessRequest(header, objects);
				return true;
			}
			else
			{
				return false;
			}
		}
		else
		{
			this->ProcessRequest(header, objects);
			return true;
		}
	}
}

void OContext::CheckForUnsolicited()
{
	if (this->CanTransmit() && this->unsol.IsIdle() && this->params.allowUnsolicited)
	{
		if (this->unsol.completedNull)
		{
			// are there events to be reported?
			if (this->params.unsolClassMask.Intersects(this->eventBuffer.UnwrittenClassField()))
			{

				auto response = this->unsol.tx.Start();
				auto writer = response.GetWriter();

				this->eventBuffer.Unselect();
				this->eventBuffer.SelectAllByClass(this->params.unsolClassMask);
				this->eventBuffer.Load(writer);

				build::NullUnsolicited(response, this->unsol.seq.num, this->GetResponseIIN());
				this->StartUnsolicitedConfirmTimer();
				this->unsol.pState = &OutstationUnsolicitedStateConfirmWait::Inst();
				this->BeginUnsolTx(response.ToRSlice());
			}
		}
		else
		{
			// send a NULL unsolcited message
			auto response = this->unsol.tx.Start();
			build::NullUnsolicited(response, this->unsol.seq.num, this->GetResponseIIN());
			this->StartUnsolicitedConfirmTimer();
			this->unsol.pState = &OutstationUnsolicitedStateConfirmWait::Inst();
			this->BeginUnsolTx(response.ToRSlice());
		}
	}
}

bool OContext::StartSolicitedConfirmTimer()
{
	auto timeout = [&]()
	{
		this->sol.pState = this->sol.pState->OnConfirmTimeout(*this);
		this->CheckForTaskStart();
	};
	return this->confirmTimer.Start(this->params.unsolConfirmTimeout, timeout);
}

bool OContext::StartUnsolicitedConfirmTimer()
{
	auto timeout = [this]()
	{
		this->unsol.pState = this->unsol.pState->OnConfirmTimeout(*this);
		this->CheckForTaskStart();
	};
	return this->confirmTimer.Start(this->params.unsolConfirmTimeout, timeout);
}

OutstationSolicitedStateBase* OContext::RespondToNonReadRequest(const APDUHeader& header, const openpal::RSlice& objects)
{
	this->history.RecordLastProcessedRequest(header, objects);

	auto response = this->sol.tx.Start();
	auto writer = response.GetWriter();
	response.SetFunction(FunctionCode::RESPONSE);
	response.SetControl(AppControlField(true, true, false, false, header.control.SEQ));
	auto iin = this->HandleNonReadResponse(header, objects, writer);
	response.SetIIN(iin | this->GetResponseIIN());
	this->BeginResponseTx(response.ToRSlice());
	return &OutstationSolicitedStateIdle::Inst();
}

OutstationSolicitedStateBase* OContext::RespondToReadRequest(const APDUHeader& header, const openpal::RSlice& objects)
{
	this->history.RecordLastProcessedRequest(header, objects);

	auto response = this->sol.tx.Start();
	auto writer = response.GetWriter();
	response.SetFunction(FunctionCode::RESPONSE);
	auto result = this->HandleRead(objects, writer);
	result.second.SEQ = header.control.SEQ;
	this->sol.seq.confirmNum = header.control.SEQ;
	response.SetControl(result.second);
	response.SetIIN(result.first | this->GetResponseIIN());
	this->BeginResponseTx(response.ToRSlice());

	if (result.second.CON)
	{
		this->StartSolicitedConfirmTimer();
		return &OutstationStateSolicitedConfirmWait::Inst();
	}
	else
	{
		return  &OutstationSolicitedStateIdle::Inst();
	}
}

OutstationSolicitedStateBase* OContext::ContinueMultiFragResponse(const AppSeqNum& seq)
{
	auto response = this->sol.tx.Start();
	auto writer = response.GetWriter();
	response.SetFunction(FunctionCode::RESPONSE);
	auto control = this->rspContext.LoadResponse(writer);
	control.SEQ = seq;
	this->sol.seq.confirmNum = seq;
	response.SetControl(control);
	response.SetIIN(this->GetResponseIIN());
	this->BeginResponseTx(response.ToRSlice());

	if (control.CON)
	{
		this->StartSolicitedConfirmTimer();
		return &OutstationStateSolicitedConfirmWait::Inst();
	}
	else
	{
		return &OutstationSolicitedStateIdle::Inst();
	}
}

bool OContext::CanTransmit() const
{
	return isOnline && !isTransmitting;
}

IINField OContext::GetResponseIIN()
{
	return this->staticIIN | this->GetDynamicIIN() | this->pApplication->GetApplicationIIN().ToIIN();
}

IINField OContext::GetDynamicIIN()
{
	auto classField = this->eventBuffer.UnwrittenClassField();

	IINField ret;
	ret.SetBitToValue(IINBit::CLASS1_EVENTS, classField.HasClass1());
	ret.SetBitToValue(IINBit::CLASS2_EVENTS, classField.HasClass2());
	ret.SetBitToValue(IINBit::CLASS3_EVENTS, classField.HasClass3());
	ret.SetBitToValue(IINBit::EVENT_BUFFER_OVERFLOW, this->eventBuffer.IsOverflown());

	return ret;
}

void OContext::ParseHeader(const openpal::RSlice& apdu)
{
	FORMAT_HEX_BLOCK(this->logger, flags::APP_HEX_RX, apdu, 18, 18);

	APDUHeader header;
	if (!APDUHeaderParser::ParseRequest(apdu, header, &this->logger))
	{
		return;
	}

	FORMAT_LOG_BLOCK(this->logger, flags::APP_HEADER_RX,
	                 "FIR: %i FIN: %i CON: %i UNS: %i SEQ: %i FUNC: %s",
	                 header.control.FIR,
	                 header.control.FIN,
	                 header.control.CON,
	                 header.control.UNS,
	                 header.control.SEQ,
	                 FunctionCodeToString(header.function));

	// outstations should only process single fragment messages that don't request confirmation
	if (!header.control.IsFirAndFin() || header.control.CON)
	{
		SIMPLE_LOG_BLOCK(this->logger, flags::WARN, "Ignoring fragment. Request must be FIR/FIN/!CON");
		return;
	}

	auto objects = apdu.Skip(APDU_REQUEST_HEADER_SIZE);

	// this method is virtual, and the implementation may vary for SA
	this->ReceiveParsedHeader(apdu, header, objects);
}

void OContext::CheckForTaskStart()
{
	// do these checks in order of priority
	this->CheckForDeferredRequest();
	this->CheckForUnsolicited();
}

void OContext::SetRestartIIN()
{
	this->staticIIN.SetBit(IINBit::DEVICE_RESTART);
}

IDatabase& OContext::GetDatabase()
{
	return this->database;
}

DatabaseConfigView OContext::GetConfigView()
{
	return this->database.GetConfigView();
}

//// ----------------------------- function handlers -----------------------------

void OContext::ProcessRequestNoAck(const APDUHeader& header, const openpal::RSlice& objects)
{
	switch (header.function)
	{
	case(FunctionCode::DIRECT_OPERATE_NR) :
		this->HandleDirectOperate(objects, OperateType::DirectOperateNoAck, nullptr); // no object writer, this is a no ack code
		break;
	default:
		FORMAT_LOG_BLOCK(this->logger, flags::WARN, "Ignoring NR function code: %s", FunctionCodeToString(header.function));
		break;
	}
}

IINField OContext::HandleNonReadResponse(const APDUHeader& header, const openpal::RSlice& objects, HeaderWriter& writer)
{
	switch (header.function)
	{
	case(FunctionCode::WRITE) :
		return this->HandleWrite(objects);
	case(FunctionCode::SELECT) :
		return this->HandleSelect(objects, writer);
	case(FunctionCode::OPERATE) :
		return this->HandleOperate(objects, writer);
	case(FunctionCode::DIRECT_OPERATE) :
		return this->HandleDirectOperate(objects, OperateType::DirectOperate, &writer);
	case(FunctionCode::COLD_RESTART) :
		return this->HandleRestart(objects, false, &writer);
	case(FunctionCode::WARM_RESTART) :
		return this->HandleRestart(objects, true, &writer);
	case(FunctionCode::ASSIGN_CLASS) :
		return this->HandleAssignClass(objects);
	case(FunctionCode::DELAY_MEASURE) :
		return this->HandleDelayMeasure(objects, writer);
	case(FunctionCode::DISABLE_UNSOLICITED) :
		return this->params.allowUnsolicited ? this->HandleDisableUnsolicited(objects, writer) : IINField(IINBit::FUNC_NOT_SUPPORTED);
	case(FunctionCode::ENABLE_UNSOLICITED) :
		return this->params.allowUnsolicited ? this->HandleEnableUnsolicited(objects, writer) : IINField(IINBit::FUNC_NOT_SUPPORTED);
	default:
		return	IINField(IINBit::FUNC_NOT_SUPPORTED);
	}
}

Pair<IINField, AppControlField> OContext::HandleRead(const openpal::RSlice& objects, HeaderWriter& writer)
{
	this->rspContext.Reset();
	this->eventBuffer.Unselect(); // always un-select any previously selected points when we start a new read request
	this->database.GetStaticSelector().Unselect();

	ReadHandler handler(this->logger, this->database.GetStaticSelector(), this->eventBuffer);
	auto result = APDUParser::Parse(objects, handler, &this->logger, ParserSettings::NoContents()); // don't expect range/count context on a READ
	if (result == ParseResult::OK)
	{
		auto control = this->rspContext.LoadResponse(writer);
		return Pair<IINField, AppControlField>(handler.Errors(), control);
	}
	else
	{
		this->rspContext.Reset();
		return Pair<IINField, AppControlField>(IINFromParseResult(result), AppControlField(true, true, false, false));
	}
}

IINField OContext::HandleWrite(const openpal::RSlice& objects)
{
	WriteHandler handler(this->logger, *this->pApplication, &this->staticIIN);
	auto result = APDUParser::Parse(objects, handler, &this->logger);
	return (result == ParseResult::OK) ? handler.Errors() : IINFromParseResult(result);
}

IINField OContext::HandleDirectOperate(const openpal::RSlice& objects, OperateType opType, HeaderWriter* pWriter)
{
	// since we're echoing, make sure there's enough size before beginning
	if (pWriter && (objects.Size() > pWriter->Remaining()))
	{
		FORMAT_LOG_BLOCK(this->logger, flags::WARN, "Igonring command request due to oversized payload size of %u", objects.Size());
		return IINField(IINBit::PARAM_ERROR);
	}
	else
	{
		CommandActionAdapter adapter(this->pCommandHandler, false, opType);
		CommandResponseHandler handler(this->logger, this->params.maxControlsPerRequest, &adapter, pWriter);
		auto result = APDUParser::Parse(objects, handler, &this->logger);
		return (result == ParseResult::OK) ? handler.Errors() : IINFromParseResult(result);
	}
}

IINField OContext::HandleSelect(const openpal::RSlice& objects, HeaderWriter& writer)
{
	// since we're echoing, make sure there's enough size before beginning
	if (objects.Size() > writer.Remaining())
	{
		FORMAT_LOG_BLOCK(this->logger, flags::WARN, "Igonring command request due to oversized payload size of %i", objects.Size());
		return IINField(IINBit::PARAM_ERROR);
	}
	else
	{
		// the 'OperateType' is just ignored  since it's a select
		CommandActionAdapter adapter(this->pCommandHandler, true, OperateType::DirectOperate);
		CommandResponseHandler handler(this->logger, this->params.maxControlsPerRequest, &adapter, &writer);
		auto result = APDUParser::Parse(objects, handler, &this->logger);
		if (result == ParseResult::OK)
		{
			if (handler.AllCommandsSuccessful())
			{
				this->control.Select(this->sol.seq.num, this->pExecutor->GetTime(), objects);
			}

			return handler.Errors();
		}
		else
		{
			return IINFromParseResult(result);
		}
	}
}

IINField OContext::HandleOperate(const openpal::RSlice& objects, HeaderWriter& writer)
{
	// since we're echoing, make sure there's enough size before beginning
	if (objects.Size() > writer.Remaining())
	{
		FORMAT_LOG_BLOCK(this->logger, flags::WARN, "Igonring command request due to oversized payload size of %i", objects.Size());
		return IINField(IINBit::PARAM_ERROR);
	}
	else
	{
		auto now = this->pExecutor->GetTime();
		auto result = this->control.ValidateSelection(this->sol.seq.num, now, this->params.selectTimeout, objects);

		if (result == CommandStatus::SUCCESS)
		{
			CommandActionAdapter adapter(this->pCommandHandler, false, OperateType::SelectBeforeOperate);
			CommandResponseHandler handler(this->logger, this->params.maxControlsPerRequest, &adapter, &writer);
			auto result = APDUParser::Parse(objects, handler, &this->logger);
			return (result == ParseResult::OK) ? handler.Errors() : IINFromParseResult(result);
		}
		else
		{
			return this->HandleCommandWithConstant(objects, writer, result);
		}
	}
}

IINField OContext::HandleDelayMeasure(const openpal::RSlice& objects, HeaderWriter& writer)
{
	if (objects.IsEmpty())
	{
		Group52Var2 value;
		value.time = 0; 	// respond with 0 time delay
		writer.WriteSingleValue<UInt8, Group52Var2>(QualifierCode::UINT8_CNT, value);
		return IINField::Empty();
	}
	else
	{
		// there shouldn't be any trailing headers in delay measure request, no need to even parse
		return IINField(IINBit::PARAM_ERROR);
	}
}

IINField OContext::HandleRestart(const openpal::RSlice& objects, bool isWarmRestart, HeaderWriter* pWriter)
{
	if (objects.IsEmpty())
	{
		auto mode = isWarmRestart ? this->pApplication->WarmRestartSupport() : this->pApplication->ColdRestartSupport();

		switch (mode)
		{
		case(RestartMode::UNSUPPORTED) :
			return IINField(IINBit::FUNC_NOT_SUPPORTED);
		case(RestartMode::SUPPORTED_DELAY_COARSE) :
			{
				auto delay = isWarmRestart ? this->pApplication->WarmRestart() : this->pApplication->ColdRestart();
				if (pWriter)
				{
					Group52Var1 coarse;
					coarse.time = delay;
					pWriter->WriteSingleValue<UInt8>(QualifierCode::UINT8_CNT, coarse);
				}
				return IINField::Empty();
			}
		default:
			{
				auto delay = isWarmRestart ? this->pApplication->WarmRestart() : this->pApplication->ColdRestart();
				if (pWriter)
				{
					Group52Var2 fine;
					fine.time = delay;
					pWriter->WriteSingleValue<UInt8>(QualifierCode::UINT8_CNT, fine);
				}
				return IINField::Empty();
			}
		}
	}
	else
	{
		// there shouldn't be any trailing headers in restart requests, no need to even parse
		return IINField(IINBit::PARAM_ERROR);
	}
}

IINField OContext::HandleAssignClass(const openpal::RSlice& objects)
{
	if (this->pApplication->SupportsAssignClass())
	{
		AssignClassHandler handler(this->logger, *this->pExecutor, *this->pApplication, this->database.GetClassAssigner());
		auto result = APDUParser::Parse(objects, handler, &this->logger, ParserSettings::NoContents());
		return (result == ParseResult::OK) ? handler.Errors() : IINFromParseResult(result);
	}
	else
	{
		return IINField(IINBit::FUNC_NOT_SUPPORTED);
	}
}

IINField OContext::HandleDisableUnsolicited(const openpal::RSlice& objects, HeaderWriter& writer)
{
	ClassBasedRequestHandler handler(this->logger);
	auto result = APDUParser::Parse(objects, handler, &this->logger);
	if (result == ParseResult::OK)
	{
		this->params.unsolClassMask.Clear(handler.GetClassField());
		return handler.Errors();
	}
	else
	{
		return IINFromParseResult(result);
	}
}

IINField OContext::HandleEnableUnsolicited(const openpal::RSlice& objects, HeaderWriter& writer)
{
	ClassBasedRequestHandler handler(this->logger);
	auto result = APDUParser::Parse(objects, handler, &this->logger);
	if (result == ParseResult::OK)
	{
		this->params.unsolClassMask.Set(handler.GetClassField());
		return handler.Errors();
	}
	else
	{
		return IINFromParseResult(result);
	}
}

IINField OContext::HandleCommandWithConstant(const openpal::RSlice& objects, HeaderWriter& writer, CommandStatus status)
{
	ConstantCommandAction constant(status);
	CommandResponseHandler handler(this->logger, this->params.maxControlsPerRequest, &constant, &writer);
	auto result = APDUParser::Parse(objects, handler, &this->logger);
	return IINFromParseResult(result);
}


}

