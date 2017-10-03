// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MIDIS_SEQ_HANDLER_H_
#define MIDIS_SEQ_HANDLER_H_

#include <memory>

#include <base/memory/weak_ptr.h>
#include <brillo/message_loops/message_loop.h>
#include <gtest/gtest_prod.h>

#include "midis/device.h"
#include "midis/device_tracker.h"
#include "midis/seq_handler_interface.h"

namespace midis {

class DeviceTracker;

// Class to handle all interactions with the ALSA sequencer interface.
// NOTE: The term "input" refers to data received *from* the MIDI H/W and
// external clients that are registered to the ALSA sequencer interfance. The
// term "output" refers to data that a client of midis write *to* MIDI H/W and
// external clients.
class SeqHandler : public SeqHandlerInterface {
 public:
  using AddDeviceCallback = base::Callback<void(std::unique_ptr<Device>)>;
  using RemoveDeviceCallback = base::Callback<void(uint32_t, uint32_t)>;
  using HandleReceiveDataCallback =
      base::Callback<void(uint32_t, uint32_t, uint32_t, const char*, size_t)>;
  using IsDevicePresentCallback = base::Callback<bool(uint32_t, uint32_t)>;
  using IsPortPresentCallback =
      base::Callback<bool(uint32_t, uint32_t, uint32_t)>;

  struct SeqDeleter {
    void operator()(snd_seq_t* seq) const { snd_seq_close(seq); }
  };

  struct MidiEventDeleter {
    void operator()(snd_midi_event_t* coder) const {
      snd_midi_event_free(coder);
    }
  };

  using ScopedMidiEventPtr =
      std::unique_ptr<snd_midi_event_t, MidiEventDeleter>;
  using ScopedSeqPtr = std::unique_ptr<snd_seq_t, SeqDeleter>;

  SeqHandler(AddDeviceCallback add_device_cb,
             RemoveDeviceCallback remove_device_cb,
             HandleReceiveDataCallback handle_rx_data_cb,
             IsDevicePresentCallback is_device_present_cb,
             IsPortPresentCallback is_port_present_cb);

  ~SeqHandler() override = default;

  // Initializes the ALSA seq interface. Creates client handles for input and
  // output, as well create an input port to receive messages (announce as
  // well as MIDI data) from ALSA seq. Also starts off the file watcher which
  // watches for events on the input port.
  bool InitSeq() override;
  void ProcessAlsaClientFd() override;

  // Creates a Device object and runs the necessary callback to register that
  // object with DeviceTracker, stored in |add_device_cb_|
  void AddSeqDevice(uint32_t device_id) override;

  // At present, we don't support hotplugging of individual ports in devices.
  // so, we enumerate all the available ports in AddAlsaDevice().
  // This function is here merely to handle MIDI events associated with any
  // port being added or removed later (and to print an error message, since
  // we don't support it yet)
  void AddSeqPort(uint32_t device_id, uint32_t port_id) override;

  // Runs the relevant callback, stored in |remove_device_cb_|, when a MIDI H/W
  // device or external client is removed from the ALSA sequencer interface.
  void RemoveSeqDevice(uint32_t device_id) override;

  void RemoveSeqPort(uint32_t device_id, uint32_t port_id) override;

  // Callback to run when starting an input port (establishes a subscription,
  // and creates a relevant port on the server side, in necessary).
  // Returns true on success, false otherwise.
  bool SubscribeInPort(uint32_t device_id, uint32_t port_id) override;

  // Callback to run when starting an input port (establishes a subscription,
  // and creates a relevant port on the server side, in necessary).
  // Returns created seq port id success, -1 otherwise.
  int SubscribeOutPort(uint32_t device_id, uint32_t port_id) override;

  // The following two functions undo the work of the Subscribe*Port() function,
  // for input and output ports respectively.
  void UnsubscribeInPort(uint32_t device_id, uint32_t port_id) override;
  void UnsubscribeOutPort(int out_port_id) override;

  // Encodes the bytes in a MIDI buffer into the provided |encoder|.
  bool EncodeMidiBytes(int out_port_id,
                       snd_seq_t* out_client,
                       const uint8_t* buffer,
                       size_t buffer_len,
                       snd_midi_event_t* encoder);

  // Callback to send MIDI data to the H/W. This callback is generally called by
  // a Device handler which receives MIDI data from a client (e.g ARC++). The
  // Device handler will in turn be called by a Client handler which is
  // listening for data from it's client.
  void SendMidiData(int out_port_id,
                    const uint8_t* buffer,
                    size_t buf_len) override;

  // This function processes the MIDI data received from H/W or an external
  // client, and invokes the callback |handle_rx_data_cb_| which handles the
  // data accordingly.
  void ProcessMidiEvent(snd_seq_event_t* event) override;

  // Wrappers for functions that interact with the ALSA Sequencer interface.
  // These are kept separately, because the intention is to mock these functions
  // in unit tests.
  virtual int SndSeqEventOutputDirect(snd_seq_t* out_client,
                                      snd_seq_event_t* event);

 protected:
  // For testing purposes.
  SeqHandler();

 private:
  friend class SeqHandlerTest;
  FRIEND_TEST(SeqHandlerTest, TestEncodeBytes);

  // Enumerates all clients which are already connected to the ALSA Sequencer.
  void EnumerateExistingDevices();

  std::unique_ptr<snd_seq_t, SeqDeleter> in_client_;
  std::unique_ptr<snd_seq_t, SeqDeleter> out_client_;
  std::unique_ptr<snd_midi_event_t, MidiEventDeleter> decoder_;
  int in_client_id_;
  int out_client_id_;
  int in_port_id_;
  std::unique_ptr<pollfd> pfd_;
  brillo::MessageLoop::TaskId taskid_;

  AddDeviceCallback add_device_cb_;
  RemoveDeviceCallback remove_device_cb_;
  HandleReceiveDataCallback handle_rx_data_cb_;
  IsDevicePresentCallback is_device_present_cb_;
  IsPortPresentCallback is_port_present_cb_;
  base::WeakPtrFactory<SeqHandler> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(SeqHandler);
};

}  // namespace midis

#endif  // MIDIS_SEQ_HANDLER_H_
