#include "kernel/fs/devfs/input_node.hpp"

#include "cinux/assert.hpp"
#include "cinux/ref_count.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/device/device.hpp"
#include "kernel/driver/input/event_stream.hpp"
#include "kernel/driver/input/input_config.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {
namespace {
using driver::EventStream;
struct InputOpen : base::RefCount<> {
    EventStream& stream;
    DeviceOps    ops{};
    explicit InputOpen(EventStream& source);
    ~InputOpen() { stream.release_owner(this); }
};
base::Result<unsigned long> input_read(void* context, void* sink, unsigned long count) {
    auto* open = static_cast<InputOpen*>(context);
    return open->stream.read(sink, count, open);
}
bool input_has_input(void* context) {
    const auto* const kOpen = static_cast<InputOpen*>(context);
    return kOpen->stream.poll(kOpen);
}
long long input_ioctl(void* context, unsigned long long word, unsigned long long argument) {
    auto* open = static_cast<InputOpen*>(context);
    return open->stream.answer(open, word, argument);
}
base::Result<unsigned long> input_write(void* /*context*/, const void* /*source*/,
                                        unsigned long /*count*/) {
    return base::KernelError::kNotSupported;
}
void retain_open(void* context) {
    static_cast<InputOpen*>(context)->retain();
}
void release_open(void* context) {
    auto* open = static_cast<InputOpen*>(context);
    if (open->release()) {
        delete open;
    }
}
InputOpen::InputOpen(EventStream& source)
    : stream(source),
      ops{
          .read      = input_read,
          .write     = input_write,
          .has_input = input_has_input,
          .ioctl     = input_ioctl,
          .release   = release_open,
          .context   = this,
          .retain    = retain_open,
      } {}
base::Result<FsNode> open_minor(void* context, FsNode node, unsigned long minor) {
    auto* open  = new InputOpen(*static_cast<EventStream*>(context));
    node.opaque = &open->ops;
    node.ino    = driver::kInputInoBase + minor;
    return node;
}
constexpr unsigned long kKeyboardMinor = 64;
constexpr unsigned long kPointerMinor  = 65;
base::Result<FsNode>    open_keyboard(void* context, FsNode node) {
    return open_minor(context, node, kKeyboardMinor);
}
base::Result<FsNode> open_pointer(void* context, FsNode node) {
    return open_minor(context, node, kPointerMinor);
}
class InputDevices {
public:
    static InputDevices& self() {
        static InputDevices devices;
        return devices;
    }
    void seat(driver::EventStream& keyboard, driver::EventStream& pointer) {
        keyboard_.context = &keyboard;
        pointer_.context  = &pointer;
        seat_stream("event0", driver::kInputEventMajor, kKeyboardMinor, &keyboard_);
        seat_stream("event1", driver::kInputEventMajor, kPointerMinor, &pointer_);
    }

private:
    static void seat_stream(const char* name, unsigned long major, unsigned long minor,
                            const DeviceOps* ops) {
        const auto kSeated = driver::DeviceModel::self().seat({
            .klass       = driver::DeviceClass::kCharacter,
            .name        = name,
            .number      = MakeDeviceId(major, minor),
            .ops         = ops,
            .bus         = "virtual",
            .driver_name = "input",
        });
        base::safety::Check(kSeated.ok(), "device model refused an input seat");
    }
    DeviceOps keyboard_{.read = input_read, .write = input_write, .open = open_keyboard};
    DeviceOps pointer_{.read = input_read, .write = input_write, .open = open_pointer};
};
}
void RegisterInputDevices(driver::EventStream& keyboard, driver::EventStream& pointer) {
    InputDevices::self().seat(keyboard, pointer);
}
}
