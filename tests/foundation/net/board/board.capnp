# The serializer board's messages in Cap'n Proto (peer_capnproto.cpp writes and reads them through the C++ capnp
# generates at build time); crd-net's are tests/foundation/net/schemas/serialize_board.json, protobuf's board.proto.
@0xd4c1e0b5a3f29687;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("serialize_board::cp");

enum Stance
{
    idle @0;
    walk @1;
    run @2;
    crouch @3;
}

enum Room
{
    all @0;
    team @1;
    squad @2;
    whisper @3;
}

struct Entity
{
    id @0 :UInt16;
    x @1 :Float32;
    y @2 :Float32;
    z @3 :Float32;
    qx @4 :Float32;
    qy @5 :Float32;
    qz @6 :Float32;
    qw @7 :Float32;
    vx @8 :Float32;
    vy @9 :Float32;
    vz @10 :Float32;
    health @11 :UInt8;
    stance @12 :Stance;
    firing @13 :Bool;
    target :union
    {
        none @14 :Void;
        id @15 :UInt16;
    }
}

struct Snapshot
{
    tick @0 :UInt32;
    entities @1 :List(Entity);
}

struct Command
{
    tick @0 :UInt32;
    buttons @1 :UInt16;
    yaw @2 :Float32;
    pitch @3 :Float32;
    moveX @4 :Int8;
    moveY @5 :Int8;
}

struct Chat
{
    sender @0 :UInt16;
    room @1 :Room;
    text @2 :Text;
    mentions @3 :List(UInt16);
}
