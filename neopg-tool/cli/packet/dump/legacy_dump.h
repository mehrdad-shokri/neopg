// legacy dump format
// Copyright 2018 The NeoPG developers
//
// NeoPG is released under the Simplified BSD License (see license.txt)

#pragma once

#include <neopg-tool/cli/packet/dump_packet_sink.h>

namespace NeoPG {

/// Legacy dump format like GnuPG.
class LegacyDump : public DumpPacketSink {
 public:
  /// Dispatcher.
  void dump(const Packet* packet) const override;

  void dump(const MarkerPacket* packet) const override;
  void dump(const UserIdPacket* packet) const override;
  void dump(const UserAttributePacket* packet) const override;
  void dump(const PublicKeyPacket* packet) const override;
  void dump(const PublicSubkeyPacket* packet) const override;
  void dump(const SignaturePacket* packet) const override;

  LegacyDump(std::ostream& out) : DumpPacketSink(out) {}
};

}  // Namespace NeoPG
