@0xb41c8c4c1b2a49c8;

const protocolVersion :UInt16 = 1;

enum MsgType {
  video   @0;
  audio   @1;
  touch   @2;
  status  @3;
  control @4;
}

struct Envelope {
  version        @0 :UInt16 = .protocolVersion;  # fully-qualified
  msgType        @1 :MsgType;
  timestampUsec  @2 :UInt64;
  data           @3 :Data;
}