/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2022 University of California, Los Angeles
 *
 * This file is part of ndn-svs, synchronization library for distributed realtime
 * applications for NDN.
 *
 * ndn-svs library is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free Software
 * Foundation, in version 2.1 of the License.
 *
 * ndn-svs library is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.
 */

#include "core.hpp"
#include "global.h"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>

namespace ndn {
namespace svs {

int SVSyncCore::s_instanceCounter = 0;

const NodeID SVSyncCore::EMPTY_NODE_ID;

SVSyncCore::SVSyncCore(ndn::Face& face,
                       const Name& syncPrefix,
                       const UpdateCallback& onUpdate,
                       const SecurityOptions& securityOptions,
                       const NodeID& nid,
                       uint64_t numRand,
                       uint64_t numRecent)
  : m_face(face)
  , m_syncPrefix(syncPrefix)
  , m_securityOptions(securityOptions)
  , m_id(nid)
  , m_onUpdate(onUpdate)
  , m_rng(std::hash<std::string>()("SALT_WHATEVER"+nid.toUri()+"SALT"))
  , m_packetDist(10, 15)
  , m_retxDist(2000 * 0.9, 2000 * 1.1)
  , m_intrReplyDist(200 * 0.8, 200 * 1.2)
  , m_keyChainMem("pib-memory:", "tpm-memory:")
  , m_scheduler(m_face.getIoService())
  , m_instanceId(s_instanceCounter++)
  , m_subsetSelect(numRecent,numRand, 10,nid)
{
  // Register sync interest filter
  m_syncRegisteredPrefix =
    m_face.setInterestFilter(syncPrefix,
                             std::bind(&SVSyncCore::onSyncInterest, this, _2),
                             std::bind(&SVSyncCore::sendInitialInterest, this),
                             [] (auto&&...) {
                                NDN_THROW(Error("Failed to register sync prefix"));
                             });
}

void
SVSyncCore::sendInitialInterest()
{
  // Wait for 100ms before sending the first sync interest
  // This is necessary to give other things time to initialize
  m_scheduler.schedule(time::milliseconds(100), [this] {
    m_initialized = true;
    retxSyncInterest(true, 0);
  });
}

void
SVSyncCore::onSyncInterest(const Interest &interest)
{
    onSyncInterestValidated(interest);
    return;
}

void
SVSyncCore::onSyncInterestValidated(const Interest &interest)
{
  const auto &n = interest.getName();
  // Get state vector
  std::shared_ptr<VersionVector> vvOther;
  try
  {
    vvOther = std::make_shared<VersionVector>(n.get(-1));//changed!
  }
  catch (ndn::tlv::Error&)
  {
    return;
  }

  //if (m_recvExtraBlock && interest.hasApplicationParameters())
  //{
  //  try {
  //    m_recvExtraBlock(interest.getApplicationParameters().blockFromValue(), *vvOther);
  //  } catch (std::exception&) {}
  //}

  // Merge state vector
  bool myVectorNew, otherVectorNew;
  std::tie(myVectorNew, otherVectorNew) = mergeStateVector(*vvOther);

  // Try to record; the call will check if in suppression state
  if (recordVector(*vvOther))
    return;

  // If incoming state identical/newer to local vector, reset timer
  // If incoming state is older, send sync interest immediately
  if (!myVectorNew)
  {
    retxSyncInterest(false, 0);
  }
  else
  {
    enterSuppressionState(*vvOther);
    // Check how much time is left on the timer,
    // reset to ~m_intrReplyDist if more than that.
    int delay = m_intrReplyDist(m_rng);
    if (getCurrentTime() + delay * 1000 < m_nextSyncInterest)
    {
      retxSyncInterest(false, delay);
    }
  }
}

void
SVSyncCore::retxSyncInterest(bool send, unsigned int delay)
{
  if (send)
  {
    std::lock_guard<std::mutex> lock(m_recordedVvMutex);

    // Only send interest if in steady state or local vector has newer state
    // than recorded interests

    // Regular sendSyncInterest
    // if (!m_recordedVv || mergeStateVector(*m_recordedVv).first)
    //   sendSyncInterest();
    // m_recordedVv = nullptr;

    // sendSyncInterestFrag, fragmented by MTU, which is defined within the sendSyncInterestFrag function for now
    if (!m_recordedVv || mergeStateVector(*m_recordedVv).first){
      if (fragmentation_mtu != 0){
        sendSyncInterestFrag();
      }else{
        sendSyncInterestRandRecent(); //sendSyncInterestFrag();
      }
    }
    m_recordedVv = nullptr;
  }

  if (delay == 0)
    delay = m_retxDist(m_rng);

  {
    std::lock_guard<std::mutex> lock(m_schedulerMutex);

    // Store the scheduled time
    m_nextSyncInterest = getCurrentTime() + 1000 * delay;

    m_retxEvent = m_scheduler.schedule(time::milliseconds(delay),
                                       [this] { retxSyncInterest(true, 0); });
  }
}

void
SVSyncCore::sendSyncInterest()
{
  if (!m_initialized)
    return;

  Name syncName(m_syncPrefix);
  Interest interest;

  {
    std::lock_guard<std::mutex> lock(m_vvMutex);
    syncName.append(Name::Component(m_vv.encode()));

    // Add parameters digest
    //interest.setApplicationParameters(span<const uint8_t>{'0'});

    //if (m_getExtraBlock)
    //{
    //  interest.setApplicationParameters(m_getExtraBlock(m_vv));
    //}
  }

  interest.setName(syncName);
  interest.setInterestLifetime(time::milliseconds(1000));
  interest.setCanBePrefix(true);
  interest.setMustBeFresh(true);

  m_face.expressInterest(interest, nullptr, nullptr, nullptr);
}

void
SVSyncCore::sendSyncInterestFrag()
{
  if (!m_initialized)
    return;

  // Random select based on an artificial mtu
  std::size_t mtu = fragmentation_mtu;
  // Ceiling of integeer division

  {
    std::lock_guard<std::mutex> lock(m_vvMutex);
    // std::cerr<<"DEBUG: Size for splitted vec is "<< m_vv.encodeToVec(mtu).size()<<std::endl;
    for (auto const& single_block : m_subsetSelect.selectRandRecent(m_vv).encodeToVec(mtu)) {
      Name syncName(m_syncPrefix);
      Interest interest;
      syncName.append(Name::Component(single_block));
      interest.setName(syncName);
      interest.setInterestLifetime(time::milliseconds(300));
      interest.setCanBePrefix(true);
      interest.setMustBeFresh(true);
      total_sync_interest_count++;
      total_sync_interest_sz += interest.wireEncode().size();

      m_face.expressInterest(interest, nullptr, nullptr, nullptr);
      // std::cerr << interest << std::endl;
    }


  }
}

void
SVSyncCore::sendSyncInterestRandRecent()
{
  //assert(pRand + pRecent == 1.0 && pRand >= 0 && pRecent >= 0);

  if (!m_initialized)
    return;

  Name syncName(m_syncPrefix);
  Interest interest;

  {
    std::lock_guard<std::mutex> lock(m_vvMutex);
    // At this point the size of subselected vv is already <= MTU
    syncName.append(Name::Component(m_subsetSelect.selectRandRecent(m_vv).encode()));
  }

  interest.setName(syncName);
  interest.setInterestLifetime(time::milliseconds(300));
  interest.setCanBePrefix(true);
  interest.setMustBeFresh(true);
  total_sync_interest_count++;
  total_sync_interest_sz += interest.wireEncode().size();

  m_face.expressInterest(interest, nullptr, nullptr, nullptr);

}


void
SVSyncCore::sendSyncInterestBucket(size_t nBucketSize)
{
  if (!m_initialized)
    return;

  std::size_t mtu = this_mtu;


  m_subsetSelect.setNBucketSize(nBucketSize);
  // Pure Bucket
  m_subsetSelect.setNRecent(0);

  Name syncName(m_syncPrefix);
  Interest interest;

  {
    std::lock_guard<std::mutex> lock(m_vvMutex);
    // At this point the size of subselected vv is already <= MTU
    syncName.append(Name::Component(m_subsetSelect.selectBucketRecent(m_vv).encode()));
  }

  interest.setName(syncName);
  interest.setInterestLifetime(time::milliseconds(1000));
  interest.setCanBePrefix(true);
  interest.setMustBeFresh(true);

  m_face.expressInterest(interest, nullptr, nullptr, nullptr);
}



void
SVSyncCore::sendSyncInterestBucketMix(size_t nBucketSize, size_t nRecent)
{
  if (!m_initialized)
    return;

  std::size_t mtu = this_mtu;


  m_subsetSelect.setNBucketSize(nBucketSize);
  m_subsetSelect.setNRecent(nRecent);

  Name syncName(m_syncPrefix);
  Interest interest;

  {
    std::lock_guard<std::mutex> lock(m_vvMutex);
    // At this point the size of subselected vv is already <= MTU
    syncName.append(Name::Component(m_subsetSelect.selectBucketRecent(m_vv).encode()));
  }

  interest.setName(syncName);
  interest.setInterestLifetime(time::milliseconds(1000));
  interest.setCanBePrefix(true);
  interest.setMustBeFresh(true);

  m_face.expressInterest(interest, nullptr, nullptr, nullptr);
}


void
SVSyncCore::sendSyncInterestBucketOrdered(size_t nBucketSize)
{
  if (!m_initialized)
    return;

  std::size_t mtu = this_mtu;


  m_subsetSelect.setNBucketSize(nBucketSize);
  // Pure Bucket
  m_subsetSelect.setNRecent(0);

  Name syncName(m_syncPrefix);
  Interest interest;

  {
    std::lock_guard<std::mutex> lock(m_vvMutex);
    // At this point the size of subselected vv is already <= MTU
    syncName.append(Name::Component(m_subsetSelect.selectBucketRecent(m_vv, true).encode()));
  }

  interest.setName(syncName);
  interest.setInterestLifetime(time::milliseconds(1000));
  interest.setCanBePrefix(true);
  interest.setMustBeFresh(true);

  m_face.expressInterest(interest, nullptr, nullptr, nullptr);
}


void
SVSyncCore::sendSyncInterestBucketOrderedMix(size_t nBucketSize, size_t nRecent)
{
  if (!m_initialized)
    return;

  std::size_t mtu = this_mtu;


  m_subsetSelect.setNBucketSize(nBucketSize);
  m_subsetSelect.setNRecent(nRecent);

  Name syncName(m_syncPrefix);
  Interest interest;

  {
    std::lock_guard<std::mutex> lock(m_vvMutex);
    // At this point the size of subselected vv is already <= MTU
    syncName.append(Name::Component(m_subsetSelect.selectBucketRecent(m_vv, true).encode()));
  }

  interest.setName(syncName);
  interest.setInterestLifetime(time::milliseconds(1000));
  interest.setCanBePrefix(true);
  interest.setMustBeFresh(true);

  m_face.expressInterest(interest, nullptr, nullptr, nullptr);
}


std::pair<bool, bool>
SVSyncCore::mergeStateVector(const VersionVector &vvOther)
{
  std::lock_guard<std::mutex> lock(m_vvMutex);

  bool myVectorNew = false,
       otherVectorNew = false;

  // New data found in vvOther
  std::vector<MissingDataInfo> v;

  // Check if other vector has newer state
  for (const auto& entry : vvOther)
  {
    NodeID nidOther = entry.first;
    SeqNo seqOther = entry.second;
    SeqNo seqCurrent = m_vv.get(nidOther);

    if (seqCurrent < seqOther)
    {
      otherVectorNew = true;

      SeqNo startSeq = m_vv.get(nidOther) + 1;
      v.push_back({nidOther, startSeq, seqOther});

      m_vv.set(nidOther, seqOther);
    }
  }

  // Callback if missing data found
  if (!v.empty())
  {
    m_onUpdate(v);
  }

  // Check if I have newer state
  for (const auto& entry : m_vv)
  {
    NodeID nid = entry.first;
    SeqNo seq = entry.second;
    SeqNo seqOther = vvOther.get(nid);

    if (seqOther < seq)
    {
      myVectorNew = true;
      break;
    }
  }

  return {myVectorNew, otherVectorNew};
}

void
SVSyncCore::reset(bool isOnInterest)
{
}

SeqNo
SVSyncCore::getSeqNo(const NodeID& nid) const
{
  std::lock_guard<std::mutex> lock(m_vvMutex);
  NodeID t_nid = (nid == EMPTY_NODE_ID) ? m_id : nid;
  return m_vv.get(t_nid);
}

void
SVSyncCore::updateSeqNo(const SeqNo& seq, const NodeID& nid)
{
  NodeID t_nid = (nid == EMPTY_NODE_ID) ? m_id : nid;

  SeqNo prev;
  {
    std::lock_guard<std::mutex> lock(m_vvMutex);
    prev = m_vv.get(t_nid);
    m_vv.set(t_nid, seq);
  }

  if (seq > prev)
    retxSyncInterest(false, 1);
}

std::set<NodeID>
SVSyncCore::getNodeIds() const
{
  std::lock_guard<std::mutex> lock(m_vvMutex);
  std::set<NodeID> sessionNames;
  for (const auto& nid : m_vv)
  {
    sessionNames.insert(nid.first);
  }
  return sessionNames;
}

long
SVSyncCore::getCurrentTime() const
{
  return std::chrono::duration_cast<std::chrono::microseconds>(
    m_steadyClock.now().time_since_epoch()).count();
}

bool
SVSyncCore::recordVector(const VersionVector &vvOther)
{
  std::lock_guard<std::mutex> lock(m_recordedVvMutex);

  if (!m_recordedVv) return false;

  std::lock_guard<std::mutex> lock1(m_vvMutex);

  for (const auto& entry : vvOther)
  {
    NodeID nidOther = entry.first;
    SeqNo seqOther = entry.second;
    SeqNo seqCurrent = m_recordedVv->get(nidOther);

    if (seqCurrent < seqOther)
    {
      m_recordedVv->set(nidOther, seqOther);
    }
  }

  return true;
}

void
SVSyncCore::enterSuppressionState(const VersionVector &vvOther)
{
  std::lock_guard<std::mutex> lock(m_recordedVvMutex);

  if (!m_recordedVv)
    m_recordedVv = std::make_unique<VersionVector>(vvOther);
}

}  // namespace svs
}  // namespace ndn
