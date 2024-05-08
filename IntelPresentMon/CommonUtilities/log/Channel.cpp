#include "Channel.h"
#include "IPolicy.h"
#include "IDriver.h"
#include "IChannelObject.h"
#include "Entry.h"
#include <concurrentqueue\blockingconcurrentqueue.h>
#include <variant>
#include <semaphore>
#include "PanicLogger.h"
#include "StackTrace.h"
#include "GlobalPolicy.h"
#include "../str/String.h"
#include "../Exception.h"

namespace pmon::util::log
{
	namespace
	{
		// command packets that can be put on the entry queue in place of log entries
		// used to control the worker thread. Each packet encodes what functionality
		// to call in the variant visit routine
		struct Packet_
		{
			std::binary_semaphore semaphore{ 0 };
			void WaitUntilProcessed() { semaphore.acquire(); }
		};
		struct FlushPacket_ : public Packet_
		{
			void Process(ChannelInternal_& channel)
			{
				channel.Flush();
				semaphore.release();
			}
		};
		struct KillPacket_ : public Packet_
		{
			void Process(ChannelInternal_& channel)
			{
				channel.SignalExit();
				semaphore.release();
			}
		};
		struct FlushEntryPointPacket_ : public Packet_
		{
			void Process(ChannelInternal_& channel)
			{
				channel.DisableTraceResolution();
				channel.Flush();
				semaphore.release();
			}
		};
		// aliases for variant / queue typenames
		using QueueElementType_ = std::variant<Entry,
			std::shared_ptr<FlushPacket_>,
			std::shared_ptr<KillPacket_>,
			std::shared_ptr<FlushEntryPointPacket_>>;
		using QueueType_ = moodycamel::BlockingConcurrentQueue<QueueElementType_>;
		// shortcut to get the underlying queue variant type when given the type-erased pointer
		// (also do some sanity checking here via assert)
		struct QueueAccessor_
		{
			static QueueType_& Access(ChannelInternal_* pChan)
			{
				assert(pChan);
				auto pQueueVoid = pChan->pEntryQueue_.get();
				assert(pQueueVoid);
				return *static_cast<QueueType_*>(pQueueVoid);
			}
		};
		QueueType_& Queue_(ChannelInternal_* pChan)
		{
			return QueueAccessor_::Access(pChan);
		}
		// internal implementation of the channel, with public functions hidden from the external interface
		// but available to packet processing functions to use
		ChannelInternal_::ChannelInternal_(std::vector<std::shared_ptr<IChannelComponent>> componentPtrs)
			:
			pEntryQueue_{ std::make_shared<QueueType_>() }
		{
			for (auto& p : componentPtrs) {
				AttachComponent_(std::move(p));
			}
			worker_ = mt::Thread(L"log-chan", [this] {
				try {
					auto visitor = [this](auto& el) {
						// log entry is handled differently than command packets
						using ElementType = std::decay_t<decltype(el)>;
						if constexpr (std::is_same_v<ElementType, Entry>) {
							Entry& entry = el;
							// process all policies, tranforming entry in-place
							for (auto& pPolicy : policyPtrs_) {
								try {
									// if any policy returns false, drop entry
									if (!pPolicy->TransformFilter(entry)) {
										return;
									}
								}
								catch (...) {
									pmlog_panic_(ReportExceptionWide());
								}
							}
							// resolve trace if one is present
							if (entry.pTrace_ && !entry.pTrace_->Resolved()) {
								try {
									if (resolvingTraces_) {
										entry.pTrace_->Resolve();
									}
								}
								catch (...) {
									pmlog_panic_(ReportExceptionWide());
								}
							}
							// submit entry to all drivers (by copy)
							for (auto& pDriver : driverPtrs_) {
								try { pDriver->Submit(entry); }
								catch (...) {
									pmlog_panic_(ReportExceptionWide());
								}
							}
							if (driverPtrs_.empty()) {
								pmlog_panic_(L"No drivers in logging channel while processing entry");
							}
						}
						// if not log entry object, then shared_ptr to a command packet w/ Process member
						else {
							el->Process(*this);
						}
					};
					QueueElementType_ el;
					while (!exiting_) {
						Queue_(this).wait_dequeue(el);
						std::lock_guard lk{ mtx_ };
						std::visit(visitor, el);
					}
				}
				catch (...) {
					pmlog_panic_(ReportExceptionWide());
				}
			});
		}
 		ChannelInternal_::~ChannelInternal_() = default;
		void ChannelInternal_::Flush()
		{
			for (auto& pDriver : driverPtrs_) {
				pDriver->Flush();
			}
		}
		void ChannelInternal_::SignalExit()
		{
			exiting_ = true;
		}
		void ChannelInternal_::DisableTraceResolution()
		{
			resolvingTraces_ = false;
		}
		void ChannelInternal_::AttachComponentBlocking(std::shared_ptr<IChannelComponent> pComponent)
		{
			std::lock_guard lk{ mtx_ };
			AttachComponent_(std::move(pComponent));
		}
		void ChannelInternal_::AttachComponent_(std::shared_ptr<IChannelComponent> pComponent)
		{
			if (auto pDriver = std::dynamic_pointer_cast<IDriver>(pComponent)) {
				driverPtrs_.push_back(std::move(pDriver));
			}
			else if (auto pPolicy = std::dynamic_pointer_cast<IPolicy>(pComponent)) {
				policyPtrs_.push_back(std::move(pPolicy));
			}
			else if (auto pObject = std::dynamic_pointer_cast<IChannelObject>(pComponent)) {
				objectPtrs_.push_back(std::move(pObject));
			}
			else {
				throw Except<Exception>("bad type for component attachment in channel");
			}
		}
		void ChannelInternal_::EnqueueEntry(Entry&& e)
		{
			Queue_(this).enqueue(std::move(e));
		}
		void ChannelInternal_::EnqueueEntry(const Entry& e)
		{
			Queue_(this).enqueue(e);
		}
		template<class P, typename ...Args>
		void ChannelInternal_::EnqueuePacketWait(Args&& ...args)
		{
			auto pPacket = std::make_shared<P>(std::forward<Args>(args)...);
			Queue_(this).enqueue(pPacket);
			pPacket->WaitUntilProcessed();
		}
		template<class P, typename ...Args>
		void ChannelInternal_::EnqueuePacketAsync(Args&& ...args)
		{
			Queue_(this).enqueue(std::make_shared<P>(std::forward<Args>(args)...));
		}
	}


	// implementation of external interfaces
	Channel::Channel(std::vector<std::shared_ptr<IChannelComponent>> componentPtrs)
		:
		ChannelInternal_{ std::move(componentPtrs) }
	{}
	Channel::~Channel()
	{
		try {
			EnqueuePacketAsync<KillPacket_>();
		}
		catch (...) {
			pmlog_panic_(L"Failure enqueing kill packet in Channel dtor");
		}
	}
	void Channel::Submit(Entry&& e) noexcept
	{
		try {
			EnqueueEntry(std::move(e));
		}
		catch (...) {
			pmlog_panic_(L"Exception thrown in Channel::Submit (move)");
		}
	}
	void Channel::Submit(const Entry& e) noexcept
	{
		try {
			EnqueueEntry(e);
		}
		catch (...) {
			pmlog_panic_(L"Exception thrown in Channel::Submit (copy)");
		}
	}
	void Channel::Flush()
	{
		EnqueuePacketWait<FlushPacket_>();
	}
	void Channel::AttachComponent(std::shared_ptr<IChannelComponent> pComponent)
	{
		AttachComponentBlocking(std::move(pComponent));
	}
	void Channel::FlushEntryPointExit()
	{
		EnqueuePacketWait<FlushEntryPointPacket_>();
	}
}