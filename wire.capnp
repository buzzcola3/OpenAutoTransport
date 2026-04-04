@0xb41c8c4c1b2a49c8;

const protocolVersion :UInt16 = 1;

enum MsgType {
  video          @0;
  mediaAudio     @1;
  touch          @2;
  control        @3;
  guidanceAudio  @4;
  systemAudio    @5;
  sensor         @6;
  heartbeat      @7;
  microphoneAudio @8;
  configuration  @9;
  log            @10;
}

struct Envelope {
  version        @0 :UInt16 = .protocolVersion;  # fully-qualified
  msgType        @1 :MsgType;
  timestampUsec  @2 :UInt64;
  data           @3 :Data;
}