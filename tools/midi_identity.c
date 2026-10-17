// macOS CoreMIDI identity probe. The only transmitted bytes are the universal
// identity inquiry; there are no firmware-update or arbitrary-message options.
// clang tools/midi_identity.c -framework CoreMIDI -framework CoreFoundation -o /tmp/rhythm-midi-identity
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char reply[1024];
static size_t used;
static int complete;

static void receive(const MIDIPacketList *list, void *context, void *source) {
    (void)context; (void)source;
    pthread_mutex_lock(&lock);
    const MIDIPacket *packet = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; i++, packet = MIDIPacketNext(packet)) {
        for (UInt16 j = 0; j < packet->length; j++) {
            unsigned char b = packet->data[j];
            if (b >= 0xf8) continue; // Ignore realtime MIDI interleaved in SysEx.
            if (complete) continue;
            if (b == 0xf0) used = 0;
            if (used == 0 && b != 0xf0) continue;
            if (used >= sizeof(reply)) { used = 0; continue; }
            reply[used++] = b;
            if (b == 0xf7) {
                if (used >= 6 && reply[1] == 0x7e && reply[3] == 6 && reply[4] == 2)
                    complete = 1;
                else used = 0;
            }
        }
    }
    pthread_mutex_unlock(&lock);
}

static void name(MIDIObjectRef object, char *buffer, size_t length) {
    CFStringRef value = NULL;
    buffer[0] = 0;
    if (MIDIObjectGetStringProperty(object, kMIDIPropertyDisplayName, &value) == noErr && value) {
        CFStringGetCString(value, buffer, length, kCFStringEncodingUTF8);
        CFRelease(value);
    }
}

static void check(OSStatus status, const char *operation) {
    if (status != noErr) {
        fprintf(stderr, "%s failed: %d\n", operation, (int)status);
        exit(1);
    }
}

int main(void) {
    MIDIClientRef client = 0;
    MIDIPortRef input = 0, output = 0;
    MIDIEndpointRef source = 0, destination = 0;
    int sources = 0, destinations = 0;
    check(MIDIClientCreate(CFSTR("Rhythm identity inquiry"), NULL, NULL, &client), "MIDIClientCreate");
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++) {
        MIDIEndpointRef endpoint = MIDIGetSource(i); char label[512];
        name(endpoint, label, sizeof(label));
        if (strstr(label, "Circuit Rhythm")) {
            printf("Source: %s\n", label); source = endpoint; sources++;
        }
    }
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); i++) {
        MIDIEndpointRef endpoint = MIDIGetDestination(i); char label[512];
        name(endpoint, label, sizeof(label));
        if (strstr(label, "Circuit Rhythm")) {
            printf("Destination: %s\n", label); destination = endpoint; destinations++;
        }
    }
    if (sources != 1 || destinations != 1) {
        fprintf(stderr, "Expected exactly one Rhythm source and destination, found %d/%d; sent nothing.\n", sources, destinations);
        MIDIClientDispose(client); return 2;
    }
    check(MIDIInputPortCreate(client, CFSTR("Identity input"), receive, NULL, &input), "MIDIInputPortCreate");
    check(MIDIOutputPortCreate(client, CFSTR("Identity output"), &output), "MIDIOutputPortCreate");
    check(MIDIPortConnectSource(input, source, NULL), "MIDIPortConnectSource");
    const unsigned char inquiry[] = {0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7};
    MIDIPacketList packets;
    MIDIPacket *packet = MIDIPacketListInit(&packets);
    if (!MIDIPacketListAdd(&packets, sizeof(packets), packet, 0, sizeof(inquiry), inquiry)) return 1;
    check(MIDISend(output, destination, &packets), "MIDISend");
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 3.0, false);
    pthread_mutex_lock(&lock);
    if (complete) {
        printf("Identity:");
        for (size_t i = 0; i < used; i++) printf(" %02X", reply[i]);
        printf("\n");
        if (used == 17 && reply[5] == 0 && reply[6] == 0x20 && reply[7] == 0x29) {
            printf("Novation family/model bytes: %02X %02X %02X %02X\n", reply[8], reply[9], reply[10], reply[11]);
            if (reply[9] == 0x63 && reply[10] == 0 && reply[11] == 0) {
                if (reply[8] == 0x01) printf("Identity matches observed Rhythm application family\n");
                else if (reply[8] == 0x11) printf("Identity matches observed Rhythm bootloader family\n");
            }
            printf("Build (upstream decoding): %u\n", reply[14] * 128 + reply[15]);
        }
    } else fprintf(stderr, "No identity reply within 3 seconds.\n");
    pthread_mutex_unlock(&lock);
    MIDIPortDisconnectSource(input, source);
    MIDIPortDispose(input); MIDIPortDispose(output); MIDIClientDispose(client);
    return complete ? 0 : 3;
}
