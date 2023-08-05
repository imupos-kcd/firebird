/*
 *  The contents of this file are subject to the Initial
 *  Developer's Public License Version 1.0 (the "License");
 *  you may not use this file except in compliance with the
 *  License. You may obtain a copy of the License at
 *  http://www.ibphoenix.com/main.nfs?a=ibphoenix&page=ibp_idpl.
 *
 *  Software distributed under the License is distributed AS IS,
 *  WITHOUT WARRANTY OF ANY KIND, either express or implied.
 *  See the License for the specific language governing rights
 *  and limitations under the License.
 *
 *  The Original Code was created by Adriano dos Santos Fernandes
 *  for the Firebird Open Source RDBMS project.
 *
 *  Copyright (c) 2020 Adriano dos Santos Fernandes <adrianosf@gmail.com>
 *  and all contributors signed below.
 *
 *  All Rights Reserved.
 *  Contributor(s): ______________________________________.
 */

#include "firebird.h"
#include "../jrd/ProfilerManager.h"
#include "../jrd/Record.h"
#include "../jrd/ini.h"
#include "../jrd/tra.h"
#include "../jrd/ids.h"
#include "../jrd/recsrc/Cursor.h"
#include "../dsql/BoolNodes.h"
#include "../jrd/dpm_proto.h"
#include "../jrd/lck_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/pag_proto.h"
#include "../jrd/tra_proto.h"

using namespace Jrd;
using namespace Firebird;


//--------------------------------------


namespace
{
	class ProfilerIpc final : public IpcObject
	{
	public:
		enum class Tag : UCHAR
		{
			NOP = 0,

			RESPONSE,
			EXCEPTION,

			CANCEL_SESSION,
			DISCARD,
			FINISH_SESSION,
			FLUSH,
			PAUSE_SESSION,
			RESUME_SESSION,
			SET_FLUSH_INTERVAL,
			START_SESSION
		};

		class Guard
		{
		public:
			explicit Guard(ProfilerIpc* ipc)
				: sharedMemory(ipc->sharedMemory)
			{
				sharedMemory->mutexLock();
			}

			~Guard()
			{
				sharedMemory->mutexUnlock();
			}

			Guard(const Guard&) = delete;
			Guard& operator=(const Guard&) = delete;

		private:
			SharedMemoryBase* const sharedMemory;
		};

		struct Header : public MemoryHeader
		{
			event_t serverEvent;
			event_t clientEvent;
			USHORT bufferSize;
			Tag tag;
			char userName[USERNAME_LENGTH + 1];	// \0 if has PROFILE_ANY_ATTACHMENT
			alignas(FB_ALIGNMENT) UCHAR buffer[4096];
		};

		static const USHORT VERSION = 1;

	public:
		ProfilerIpc(thread_db* tdbb, MemoryPool& pool, AttNumber aAttachmentId);

		ProfilerIpc(const ProfilerIpc&) = delete;
		ProfilerIpc& operator=(const ProfilerIpc&) = delete;

	public:
		bool initialize(SharedMemoryBase* sm, bool init) override;
		void mutexBug(int osErrorCode, const char* text) override;

		USHORT getType() const override
		{
			return SharedMemoryBase::SRAM_PROFILER;
		}

		USHORT getVersion() const override
		{
			return VERSION;
		}

		const char* getName() const override
		{
			return "ProfilerManager";
		}

	public:
		template <typename Input, typename Output>
		void sendAndReceive(thread_db* tdbb, Tag tag, const Input* in, Output* out)
		{
			static_assert(sizeof(*in) <= sizeof(std::declval<Header>().buffer), "Buffer size too small");
			internalSendAndReceive(tdbb, tag, in, sizeof(*in), out, sizeof(*out));
		}

		template <typename Input>
		void send(thread_db* tdbb, Tag tag, const Input* in)
		{
			static_assert(sizeof(*in) <= sizeof(std::declval<Header>().buffer), "Buffer size too small");
			internalSendAndReceive(tdbb, tag, in, sizeof(*in), nullptr, 0);
		}

	private:
		void internalSendAndReceive(thread_db* tdbb, Tag tag, const void* in, unsigned inSize, void* out, unsigned outSize);

	public:
		AutoPtr<SharedMemory<Header>> sharedMemory;
		AttNumber attachmentId;
	};
}	// anonymous namespace


class Jrd::ProfilerListener final
{
public:
	explicit ProfilerListener(thread_db* tdbb);
	~ProfilerListener();

	ProfilerListener(const ProfilerListener&) = delete;
	ProfilerListener& operator=(const ProfilerListener&) = delete;

public:
	void exceptionHandler(const Firebird::Exception& ex, ThreadFinishSync<ProfilerListener*>::ThreadRoutine* routine);

private:
	void watcherThread();

	static void watcherThread(ProfilerListener* listener)
	{
		listener->watcherThread();
	}

	void processCommand(thread_db* tdbb);

private:
	Attachment* const attachment;
	Firebird::Semaphore startupSemaphore;
	ThreadFinishSync<ProfilerListener*> cleanupSync;
	Firebird::AutoPtr<ProfilerIpc> ipc;
	bool exiting = false;
};


//--------------------------------------


IExternalResultSet* ProfilerPackage::discardProcedure(ThrowStatusExceptionWrapper* /*status*/,
	IExternalContext* context, const DiscardInput::Type* in, void* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	if (!in->attachmentIdNull && AttNumber(in->attachmentId) != attachment->att_attachment_id)
	{
		ProfilerIpc ipc(tdbb, *getDefaultMemoryPool(), in->attachmentId);
		ipc.send(tdbb, ProfilerIpc::Tag::DISCARD, in);
		return nullptr;
	}

	const auto profilerManager = attachment->getProfilerManager(tdbb);

	profilerManager->discard();

	return nullptr;
}

IExternalResultSet* ProfilerPackage::flushProcedure(ThrowStatusExceptionWrapper* /*status*/,
	IExternalContext* context, const FlushInput::Type* in, void* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	if (!in->attachmentIdNull && AttNumber(in->attachmentId) != attachment->att_attachment_id)
	{
		ProfilerIpc ipc(tdbb, *getDefaultMemoryPool(), in->attachmentId);
		ipc.send(tdbb, ProfilerIpc::Tag::FLUSH, in);
		return nullptr;
	}

	const auto profilerManager = attachment->getProfilerManager(tdbb);

	profilerManager->flush();

	return nullptr;
}

IExternalResultSet* ProfilerPackage::cancelSessionProcedure(ThrowStatusExceptionWrapper* /*status*/,
	IExternalContext* context, const CancelSessionInput::Type* in, void* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	if (!in->attachmentIdNull && AttNumber(in->attachmentId) != attachment->att_attachment_id)
	{
		ProfilerIpc ipc(tdbb, *getDefaultMemoryPool(), in->attachmentId);
		ipc.send(tdbb, ProfilerIpc::Tag::CANCEL_SESSION, in);
		return nullptr;
	}

	const auto transaction = tdbb->getTransaction();
	const auto profilerManager = attachment->getProfilerManager(tdbb);

	profilerManager->cancelSession();

	return nullptr;
}

IExternalResultSet* ProfilerPackage::finishSessionProcedure(ThrowStatusExceptionWrapper* /*status*/,
	IExternalContext* context, const FinishSessionInput::Type* in, void* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	if (!in->attachmentIdNull && AttNumber(in->attachmentId) != attachment->att_attachment_id)
	{
		ProfilerIpc ipc(tdbb, *getDefaultMemoryPool(), in->attachmentId);
		ipc.send(tdbb, ProfilerIpc::Tag::FINISH_SESSION, in);
		return nullptr;
	}

	const auto profilerManager = attachment->getProfilerManager(tdbb);

	profilerManager->finishSession(tdbb, in->flush);

	return nullptr;
}

IExternalResultSet* ProfilerPackage::pauseSessionProcedure(ThrowStatusExceptionWrapper* /*status*/,
	IExternalContext* context, const PauseSessionInput::Type* in, void* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	if (!in->attachmentIdNull && AttNumber(in->attachmentId) != attachment->att_attachment_id)
	{
		ProfilerIpc ipc(tdbb, *getDefaultMemoryPool(), in->attachmentId);
		ipc.send(tdbb, ProfilerIpc::Tag::PAUSE_SESSION, in);
		return nullptr;
	}

	const auto profilerManager = attachment->getProfilerManager(tdbb);

	profilerManager->pauseSession(in->flush);

	return nullptr;
}

IExternalResultSet* ProfilerPackage::resumeSessionProcedure(ThrowStatusExceptionWrapper* /*status*/,
	IExternalContext* context, const ResumeSessionInput::Type* in, void* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	if (!in->attachmentIdNull && AttNumber(in->attachmentId) != attachment->att_attachment_id)
	{
		ProfilerIpc ipc(tdbb, *getDefaultMemoryPool(), in->attachmentId);
		ipc.send(tdbb, ProfilerIpc::Tag::RESUME_SESSION, in);
		return nullptr;
	}

	const auto profilerManager = attachment->getProfilerManager(tdbb);

	profilerManager->resumeSession();

	return nullptr;
}

IExternalResultSet* ProfilerPackage::setFlushIntervalProcedure(ThrowStatusExceptionWrapper* /*status*/,
	IExternalContext* context, const SetFlushIntervalInput::Type* in, void* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	if (!in->attachmentIdNull && AttNumber(in->attachmentId) != attachment->att_attachment_id)
	{
		ProfilerIpc ipc(tdbb, *getDefaultMemoryPool(), in->attachmentId);
		ipc.send(tdbb, ProfilerIpc::Tag::SET_FLUSH_INTERVAL, in);
		return nullptr;
	}

	const auto profilerManager = attachment->getProfilerManager(tdbb);

	profilerManager->setFlushInterval(in->flushInterval);

	return nullptr;
}

void ProfilerPackage::startSessionFunction(ThrowStatusExceptionWrapper* /*status*/,
	IExternalContext* context, const StartSessionInput::Type* in, StartSessionOutput::Type* out)
{
	const auto tdbb = JRD_get_thread_data();
	const auto attachment = tdbb->getAttachment();

	if (!in->attachmentIdNull && AttNumber(in->attachmentId) != attachment->att_attachment_id)
	{
		ProfilerIpc ipc(tdbb, *getDefaultMemoryPool(), in->attachmentId);
		ipc.sendAndReceive(tdbb, ProfilerIpc::Tag::START_SESSION, in, out);
		return;
	}

	const string description(in->description.str, in->descriptionNull ? 0 : in->description.length);
	const Nullable<SLONG> flushInterval(in->flushIntervalNull ?
		Nullable<SLONG>() : Nullable<SLONG>(in->flushInterval));
	const PathName pluginName(in->pluginName.str, in->pluginNameNull ? 0 : in->pluginName.length);
	const string pluginOptions(in->pluginOptions.str, in->pluginOptionsNull ? 0 : in->pluginOptions.length);

	const auto profilerManager = attachment->getProfilerManager(tdbb);

	out->sessionIdNull = FB_FALSE;
	out->sessionId = profilerManager->startSession(tdbb, flushInterval, pluginName, description, pluginOptions);
}


//--------------------------------------


ProfilerManager::ProfilerManager(thread_db* tdbb)
	: activePlugins(*tdbb->getAttachment()->att_pool)
{
	const auto attachment = tdbb->getAttachment();

	flushTimer = FB_NEW TimerImpl();

	flushTimer->setOnTimer([this, attachment](auto) {
		FbLocalStatus statusVector;
		EngineContextHolder innerTdbb(&statusVector, attachment->getInterface(), FB_FUNCTION);

		flush(false);
		updateFlushTimer(false);
	});
}

ProfilerManager::~ProfilerManager()
{
	flushTimer->stop();
}

ProfilerManager* ProfilerManager::create(thread_db* tdbb)
{
	return FB_NEW_POOL(*tdbb->getAttachment()->att_pool) ProfilerManager(tdbb);
}

int ProfilerManager::blockingAst(void* astObject)
{
	const auto attachment = static_cast<Attachment*>(astObject);

	try
	{
		const auto dbb = attachment->att_database;
		AsyncContextHolder tdbb(dbb, FB_FUNCTION, attachment->att_profiler_listener_lock);

		const auto profilerManager = attachment->getProfilerManager(tdbb);

		if (!profilerManager->listener)
			profilerManager->listener = FB_NEW_POOL(*attachment->att_pool) ProfilerListener(tdbb);

		LCK_release(tdbb, attachment->att_profiler_listener_lock);
	}
	catch (const Exception&)
	{} // no-op

	return 0;
}

SINT64 ProfilerManager::startSession(thread_db* tdbb, Nullable<SLONG> flushInterval,
	const PathName& pluginName, const string& description, const string& options)
{
	if (flushInterval.isAssigned())
		checkFlushInterval(flushInterval.value);

	AutoSetRestore<bool> pauseProfiler(&paused, true);

	const auto attachment = tdbb->getAttachment();
	ThrowLocalStatus status;

	const auto timestamp = TimeZoneUtil::getCurrentTimeStamp(attachment->att_current_timezone);

	if (currentSession)
	{
		currentSession->pluginSession->finish(&status, timestamp);
		currentSession = nullptr;
	}

	auto pluginPtr = activePlugins.get(pluginName);

	AutoPlugin<IProfilerPlugin> plugin;

	if (pluginPtr)
	{
		(*pluginPtr)->addRef();
		plugin.reset(pluginPtr->get());
	}
	else
	{
		GetPlugins<IProfilerPlugin> plugins(IPluginManager::TYPE_PROFILER, pluginName.nullStr());

		if (!plugins.hasData())
		{
			string msg;
			msg.printf("Profiler plugin %s is not found", pluginName.c_str());
			(Arg::Gds(isc_random) << msg).raise();
		}

		plugin.reset(plugins.plugin());
		plugin->addRef();

		plugin->init(&status, attachment->getInterface(), (FB_UINT64) fb_utils::query_performance_frequency());

		plugin->addRef();
		activePlugins.put(pluginName)->reset(plugin.get());
	}

	AutoDispose<IProfilerSession> pluginSession = plugin->startSession(&status,
		description.c_str(),
		options.c_str(),
		timestamp);

	auto& pool = *tdbb->getAttachment()->att_pool;

	currentSession.reset(FB_NEW_POOL(pool) ProfilerManager::Session(pool));
	currentSession->pluginSession = std::move(pluginSession);
	currentSession->plugin = std::move(plugin);
	currentSession->flags = currentSession->pluginSession->getFlags();

	paused = false;

	if (flushInterval.isAssigned())
		setFlushInterval(flushInterval.value);

	return currentSession->pluginSession->getId();
}

void ProfilerManager::prepareCursor(thread_db* tdbb, Request* request, const Select* select)
{
	auto profileStatement = getStatement(request);

	if (!profileStatement)
		return;

	auto cursorId = select->getCursorId();

	if (!profileStatement->definedCursors.exist(cursorId))
	{
		currentSession->pluginSession->defineCursor(profileStatement->id, cursorId,
			select->getName().nullStr(), select->getLine(), select->getColumn());

		profileStatement->definedCursors.add(cursorId);
	}

	prepareRecSource(tdbb, request, select);
}

void ProfilerManager::prepareRecSource(thread_db* tdbb, Request* request, const AccessPath* recordSource)
{
	auto profileStatement = getStatement(request);

	if (!profileStatement)
		return;

	if (profileStatement->recSourceSequence.exist(recordSource->getRecSourceId()))
		return;

	fb_assert(profileStatement->definedCursors.exist(recordSource->getCursorId()));

	PlanEntry rootEntry;
	recordSource->getPlan(tdbb, rootEntry, 0, true);

	Array<NonPooledPair<const PlanEntry*, const PlanEntry*>> flatPlan;
	rootEntry.asFlatList(flatPlan);

	NonPooledMap<ULONG, ULONG> idSequenceMap;
	auto sequencePtr = profileStatement->cursorNextSequence.getOrPut(recordSource->getCursorId());

	for (const auto& [planEntry, parentPlanEntry] : flatPlan)
	{
		const auto cursorId = planEntry->accessPath->getCursorId();
		const auto recSourceId = planEntry->accessPath->getRecSourceId();
		idSequenceMap.put(recSourceId, ++*sequencePtr);

		ULONG parentSequence = 0;

		if (parentPlanEntry)
			parentSequence = *idSequenceMap.get(parentPlanEntry->accessPath->getRecSourceId());

		string accessPath;
		planEntry->getDescriptionAsString(accessPath);

		currentSession->pluginSession->defineRecordSource(profileStatement->id, cursorId,
			*sequencePtr, planEntry->level, accessPath.c_str(), parentSequence);

		profileStatement->recSourceSequence.put(recSourceId, *sequencePtr);
	}
}

void ProfilerManager::onRequestFinish(Request* request, Stats& stats)
{
	if (const auto profileRequestId = getRequest(request, 0))
	{
		const auto profileStatement = getStatement(request);
		const auto timestamp = TimeZoneUtil::getCurrentTimeStamp(request->req_attachment->att_current_timezone);

		LogLocalStatus status("Profiler onRequestFinish");
		currentSession->pluginSession->onRequestFinish(&status, profileStatement->id, profileRequestId,
			timestamp, &stats);

		currentSession->requests.findAndRemove(profileRequestId);
	}
}

void ProfilerManager::cancelSession()
{
	if (currentSession)
	{
		LogLocalStatus status("Profiler cancelSession");

		currentSession->pluginSession->cancel(&status);
		currentSession = nullptr;
	}
}

void ProfilerManager::finishSession(thread_db* tdbb, bool flushData)
{
	if (currentSession)
	{
		const auto attachment = tdbb->getAttachment();
		const auto timestamp = TimeZoneUtil::getCurrentTimeStamp(attachment->att_current_timezone);
		LogLocalStatus status("Profiler finish");

		currentSession->pluginSession->finish(&status, timestamp);
		currentSession = nullptr;
	}

	if (flushData)
		flush();
}

void ProfilerManager::pauseSession(bool flushData)
{
	if (currentSession)
		paused = true;

	if (flushData)
		flush();
}

void ProfilerManager::resumeSession()
{
	if (currentSession)
	{
		paused = false;
		updateFlushTimer();
	}
}

void ProfilerManager::setFlushInterval(SLONG interval)
{
	checkFlushInterval(interval);

	currentFlushInterval = (unsigned) interval;

	updateFlushTimer();
}

void ProfilerManager::discard()
{
	currentSession = nullptr;
	activePlugins.clear();
}

void ProfilerManager::flush(bool updateTimer)
{
	{	// scope
		AutoSetRestore<bool> pauseProfiler(&paused, true);

		auto pluginAccessor = activePlugins.accessor();

		for (bool hasNext = pluginAccessor.getFirst(); hasNext;)
		{
			auto& pluginName = pluginAccessor.current()->first;
			auto& plugin = pluginAccessor.current()->second;

			LogLocalStatus status("Profiler flush");
			plugin->flush(&status);

			hasNext = pluginAccessor.getNext();

			if (!currentSession || plugin.get() != currentSession->plugin.get())
				activePlugins.remove(pluginName);
		}
	}

	if (updateTimer)
		updateFlushTimer();
}

void ProfilerManager::updateFlushTimer(bool canStopTimer)
{
	if (currentSession && !paused && currentFlushInterval)
		flushTimer->reset(currentFlushInterval);
	else if (canStopTimer)
		flushTimer->stop();
}

ProfilerManager::Statement* ProfilerManager::getStatement(Request* request)
{
	if (!isActive())
		return nullptr;

	auto mainProfileStatement = currentSession->statements.get(request->getStatement()->getStatementId());

	if (mainProfileStatement)
		return mainProfileStatement;

	for (const auto* statement = request->getStatement();
		 statement && !currentSession->statements.exist(statement->getStatementId());
		 statement = statement->parentStatement)
	{
		MetaName packageName;
		MetaName routineName;
		const char* type;

		if (const auto routine = statement->getRoutine())
		{
			if (statement->procedure)
				type = "PROCEDURE";
			else if (statement->function)
				type = "FUNCTION";

			packageName = routine->getName().package;
			routineName = routine->getName().identifier;
		}
		else if (statement->triggerName.hasData())
		{
			type = "TRIGGER";
			routineName = statement->triggerName;
		}
		else
			type = "BLOCK";

		const StmtNumber parentStatementId = statement->parentStatement ?
			statement->parentStatement->getStatementId() : 0;

		LogLocalStatus status("Profiler defineStatement");
		currentSession->pluginSession->defineStatement(&status,
			(SINT64) statement->getStatementId(), (SINT64) parentStatementId,
			type, packageName.nullStr(), routineName.nullStr(),
			(statement->sqlText.hasData() ? statement->sqlText->c_str() : ""));

		auto profileStatement = currentSession->statements.put(statement->getStatementId());
		profileStatement->id = statement->getStatementId();

		if (!mainProfileStatement)
			mainProfileStatement = profileStatement;
	}

	return mainProfileStatement;
}


//--------------------------------------


ProfilerIpc::ProfilerIpc(thread_db* tdbb, MemoryPool& pool, AttNumber aAttachmentId)
	: attachmentId(aAttachmentId)
{
	const auto database = tdbb->getDatabase();

	string fileName;
	static_assert(std::is_same<decltype(attachmentId), FB_UINT64>::value);
	fileName.printf(PROFILER_FILE, database->getUniqueFileId().c_str(), attachmentId);

	try
	{
		sharedMemory = FB_NEW_POOL(pool) SharedMemory<Header>(fileName.c_str(), sizeof(Header), this);
	}
	catch (const Exception& ex)
	{
		iscLogException("ProfilerManager: cannot initialize the shared memory region", ex);
		throw;
	}

	checkHeader(sharedMemory->getHeader());
}

bool ProfilerIpc::initialize(SharedMemoryBase* sm, bool init)
{
	if (init)
	{
		const auto header = reinterpret_cast<Header*>(sm->sh_mem_header);

		// Initialize the shared data header.
		initHeader(header);

		if (sm->eventInit(&header->serverEvent) != FB_SUCCESS)
			(Arg::Gds(isc_random) << "ProfilerIpc eventInit(serverEvent) failed").raise();

		if (sm->eventInit(&header->clientEvent) != FB_SUCCESS)
		{
			sm->eventFini(&header->serverEvent);
			(Arg::Gds(isc_random) << "ProfilerIpc eventInit(clientEvent) failed").raise();
		}
	}

	return true;
}

void ProfilerIpc::mutexBug(int osErrorCode, const char* text)
{
	iscLogStatus("Error when working with profiler shared memory",
		(Arg::Gds(isc_sys_request) << text << Arg::OsError(osErrorCode)).value());
}

void ProfilerIpc::internalSendAndReceive(thread_db* tdbb, Tag tag,
	const void* in, unsigned inSize, void* out, unsigned outSize)
{
	const auto attachment = tdbb->getAttachment();

	{	// scope
		ThreadStatusGuard tempStatus(tdbb);

		Lock tempLock(tdbb, sizeof(SINT64), LCK_attachment);
		tempLock.setKey(attachmentId);

		// Check if attachment is alive.
		if (LCK_lock(tdbb, &tempLock, LCK_EX, LCK_NO_WAIT))
		{
			LCK_release(tdbb, &tempLock);
			(Arg::Gds(isc_random) << "Cannot start remote profile session - attachment is not active").raise();
		}

		// Ask remote attachment to initialize the profile listener.

		tempLock.lck_type = LCK_profiler_listener;

		if (LCK_lock(tdbb, &tempLock, LCK_SR, LCK_WAIT))
			LCK_release(tdbb, &tempLock);
	}

	Guard guard(this);

	const auto header = sharedMemory->getHeader();

	header->tag = tag;

	if (attachment->locksmith(tdbb, PROFILE_ANY_ATTACHMENT))
		header->userName[0] = '\0';
	else
		strcpy(header->userName, attachment->getUserName().c_str());

	header->bufferSize = inSize;

	fb_assert(inSize <= sizeof(header->buffer));
	memcpy(header->buffer, in, inSize);

	const SLONG value = sharedMemory->eventClear(&header->clientEvent);

	sharedMemory->eventPost(&header->serverEvent);

	sharedMemory->eventWait(&header->clientEvent, value, 0);

	if (header->tag == Tag::RESPONSE)
	{
		fb_assert(outSize == header->bufferSize);
		memcpy(out, header->buffer, header->bufferSize);
	}
	else
	{
		fb_assert(header->tag == Tag::EXCEPTION);
		(Arg::Gds(isc_random) << (char*) header->buffer).raise();
	}
}


//--------------------------------------


ProfilerListener::ProfilerListener(thread_db* tdbb)
	: attachment(tdbb->getAttachment()),
	  cleanupSync(*attachment->att_pool, watcherThread, THREAD_medium)
{
	auto& pool = *attachment->att_pool;

	ipc = FB_NEW_POOL(pool) ProfilerIpc(tdbb, pool, attachment->att_attachment_id);

	cleanupSync.run(this);
}

ProfilerListener::~ProfilerListener()
{
	exiting = true;

	// Terminate the watcher thread.
	startupSemaphore.tryEnter(5);

	ProfilerIpc::Guard guard(ipc);

	auto& sharedMemory = ipc->sharedMemory;

	sharedMemory->eventPost(&sharedMemory->getHeader()->serverEvent);
	cleanupSync.waitForCompletion();

	const auto header = sharedMemory->getHeader();

	sharedMemory->eventFini(&header->serverEvent);
	sharedMemory->eventFini(&header->clientEvent);
}

void ProfilerListener::exceptionHandler(const Exception& ex, ThreadFinishSync<ProfilerListener*>::ThreadRoutine*)
{
	iscLogException("Error closing profiler watcher thread\n", ex);
}

void ProfilerListener::watcherThread()
{
	bool startup = true;

	try
	{
		while (!exiting)
		{
			auto& sharedMemory = ipc->sharedMemory;
			const auto header = sharedMemory->getHeader();

			const SLONG value = sharedMemory->eventClear(&header->serverEvent);

			if (header->tag != ProfilerIpc::Tag::NOP)
			{
				FbLocalStatus statusVector;
				EngineContextHolder tdbb(&statusVector, attachment->getInterface(), FB_FUNCTION);

				try
				{
					processCommand(tdbb);
					header->tag = ProfilerIpc::Tag::RESPONSE;
				}
				catch (const status_exception& e)
				{
					//// TODO: Serialize status vector instead of formated message.

					const ISC_STATUS* status = e.value();
					string errorMsg;
					TEXT temp[BUFFER_LARGE];

					while (fb_interpret(temp, sizeof(temp), &status))
					{
						if (errorMsg.hasData())
							errorMsg += "\n\t";

						errorMsg += temp;
					}

					header->bufferSize = MIN(errorMsg.length(), sizeof(header->buffer) - 1);
					strncpy((char*) header->buffer, errorMsg.c_str(), sizeof(header->buffer));
					header->buffer[header->bufferSize] = '\0';

					header->tag = ProfilerIpc::Tag::EXCEPTION;
				}

				sharedMemory->eventPost(&header->clientEvent);
			}

			if (startup)
			{
				startup = false;
				startupSemaphore.release();
			}

			if (exiting)
				break;

			sharedMemory->eventWait(&header->serverEvent, value, 0);
		}
	}
	catch (const Exception& ex)
	{
		iscLogException("Error in profiler watcher thread\n", ex);
	}

	try
	{
		if (startup)
			startupSemaphore.release();
	}
	catch (const Exception& ex)
	{
		exceptionHandler(ex, nullptr);
	}
}

void ProfilerListener::processCommand(thread_db* tdbb)
{
	const auto header = ipc->sharedMemory->getHeader();
	const auto profilerManager = attachment->getProfilerManager(tdbb);

	if (header->userName[0] && attachment->getUserName() != header->userName)
		status_exception::raise(Arg::Gds(isc_miss_prvlg) << "PROFILE_ANY_ATTACHMENT");

	using Tag = ProfilerIpc::Tag;

	switch (header->tag)
	{
		case Tag::CANCEL_SESSION:
			profilerManager->cancelSession();
			header->bufferSize = 0;
			break;

		case Tag::DISCARD:
			profilerManager->discard();
			header->bufferSize = 0;
			break;

		case Tag::FINISH_SESSION:
		{
			const auto in = reinterpret_cast<const ProfilerPackage::FinishSessionInput::Type*>(header->buffer);
			fb_assert(sizeof(*in) == header->bufferSize);
			profilerManager->finishSession(tdbb, in->flush);
			header->bufferSize = 0;
			break;
		}

		case Tag::FLUSH:
			profilerManager->flush();
			header->bufferSize = 0;
			break;

		case Tag::PAUSE_SESSION:
		{
			const auto in = reinterpret_cast<const ProfilerPackage::PauseSessionInput::Type*>(header->buffer);
			fb_assert(sizeof(*in) == header->bufferSize);
			profilerManager->pauseSession(in->flush);
			header->bufferSize = 0;
			break;
		}

		case Tag::RESUME_SESSION:
			profilerManager->resumeSession();
			header->bufferSize = 0;
			break;

		case Tag::SET_FLUSH_INTERVAL:
		{
			const auto in = reinterpret_cast<const ProfilerPackage::SetFlushIntervalInput::Type*>(header->buffer);
			fb_assert(sizeof(*in) == header->bufferSize);

			profilerManager->setFlushInterval(in->flushInterval);
			header->bufferSize = 0;
			break;
		}

		case Tag::START_SESSION:
		{
			const auto in = reinterpret_cast<const ProfilerPackage::StartSessionInput::Type*>(header->buffer);
			fb_assert(sizeof(*in) == header->bufferSize);

			const string description(in->description.str,
				in->descriptionNull ? 0 : in->description.length);
			const Nullable<SLONG> flushInterval(in->flushIntervalNull ?
				Nullable<SLONG>() : Nullable<SLONG>(in->flushInterval));
			const PathName pluginName(in->pluginName.str,
				in->pluginNameNull ? 0 : in->pluginName.length);
			const string pluginOptions(in->pluginOptions.str,
				in->pluginOptionsNull ? 0 : in->pluginOptions.length);

			const auto out = reinterpret_cast<ProfilerPackage::StartSessionOutput::Type*>(header->buffer);
			static_assert(sizeof(*out) <= sizeof(header->buffer), "Buffer size too small");
			header->bufferSize = sizeof(*out);

			out->sessionIdNull = FB_FALSE;
			out->sessionId = profilerManager->startSession(tdbb, flushInterval,
				pluginName, description, pluginOptions);

			break;
		}

		default:
			fb_assert(false);
			(Arg::Gds(isc_random) << "Invalid profiler's remote command").raise();
			break;
	}
}


//--------------------------------------


ProfilerPackage::ProfilerPackage(MemoryPool& pool)
	: SystemPackage(
		pool,
		"RDB$PROFILER",
		ODS_13_1,
		// procedures
		{
			SystemProcedure(
				pool,
				"CANCEL_SESSION",
				SystemProcedureFactory<CancelSessionInput, VoidMessage, cancelSessionProcedure>(),
				prc_executable,
				// input parameters
				{
					{"ATTACHMENT_ID", fld_att_id, true, "null", {blr_null}}
				},
				// output parameters
				{
				}
			),
			SystemProcedure(
				pool,
				"DISCARD",
				SystemProcedureFactory<DiscardInput, VoidMessage, discardProcedure>(),
				prc_executable,
				// input parameters
				{
					{"ATTACHMENT_ID", fld_att_id, true, "null", {blr_null}}
				},
				// output parameters
				{
				}
			),
			SystemProcedure(
				pool,
				"FINISH_SESSION",
				SystemProcedureFactory<FinishSessionInput, VoidMessage, finishSessionProcedure>(),
				prc_executable,
				// input parameters
				{
					{"FLUSH", fld_bool, false, "true", {blr_literal, blr_bool, 1}},
					{"ATTACHMENT_ID", fld_att_id, true, "null", {blr_null}}
				},
				// output parameters
				{
				}
			),
			SystemProcedure(
				pool,
				"FLUSH",
				SystemProcedureFactory<FlushInput, VoidMessage, flushProcedure>(),
				prc_executable,
				// input parameters
				{
					{"ATTACHMENT_ID", fld_att_id, true, "null", {blr_null}}
				},
				// output parameters
				{
				}
			),
			SystemProcedure(
				pool,
				"PAUSE_SESSION",
				SystemProcedureFactory<PauseSessionInput, VoidMessage, pauseSessionProcedure>(),
				prc_executable,
				// input parameters
				{
					{"FLUSH", fld_bool, false, "false", {blr_literal, blr_bool, 0}},
					{"ATTACHMENT_ID", fld_att_id, true, "null", {blr_null}}
				},
				// output parameters
				{
				}
			),
			SystemProcedure(
				pool,
				"RESUME_SESSION",
				SystemProcedureFactory<ResumeSessionInput, VoidMessage, resumeSessionProcedure>(),
				prc_executable,
				// input parameters
				{
					{"ATTACHMENT_ID", fld_att_id, true, "null", {blr_null}}
				},
				// output parameters
				{
				}
			),
			SystemProcedure(
				pool,
				"SET_FLUSH_INTERVAL",
				SystemProcedureFactory<SetFlushIntervalInput, VoidMessage, setFlushIntervalProcedure>(),
				prc_executable,
				// input parameters
				{
					{"FLUSH_INTERVAL", fld_seconds_interval, false},
					{"ATTACHMENT_ID", fld_att_id, true, "null", {blr_null}}
				},
				// output parameters
				{
				}
			)
		},
		// functions
		{
			SystemFunction(
				pool,
				"START_SESSION",
				SystemFunctionFactory<StartSessionInput, StartSessionOutput, startSessionFunction>(),
				// parameters
				{
					{"DESCRIPTION", fld_short_description, true, "null", {blr_null}},
					{"FLUSH_INTERVAL", fld_seconds_interval, true, "null", {blr_null}},
					{"ATTACHMENT_ID", fld_att_id, true, "null", {blr_null}},
					{"PLUGIN_NAME", fld_file_name2, true, "null", {blr_null}},
					{"PLUGIN_OPTIONS", fld_short_description, true, "null", {blr_null}},
				},
				{fld_prof_ses_id, false}
			)
		}
	)
{
}
