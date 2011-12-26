/* -*- Mode: C ; c-basic-offset: 2 -*- */
/*****************************************************************************
 *
 *   fluidjack - sf2 synth (fluid) with JACK MIDI input and JACK audio outputs
 *
 *   Copyright (C) 2007 Nedko Arnaudov <nedko@arnaudov.name>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 2 of the License
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 *****************************************************************************/

/* gcc `pkg-config --cflags --libs fluidsynth jack` main.c -o fluidjack */

#define _GNU_SOURCE
#include <string.h>
#include <stdbool.h>

#include <fluidsynth.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>

#define LOG_ERROR(format, arg...) fprintf(stderr, format "\n", ## arg)
#define LOG_NOTICE(format, arg...) printf(format "\n", ## arg)

struct fluidjack
{
  fluid_synth_t * synth;
  int sf_id;
  fluid_settings_t * settings;
  jack_client_t * jack_client;
  jack_port_t * in;
  jack_port_t * left;
  jack_port_t * right;
};

#define fluidjack_ptr ((struct fluidjack *)context_ptr)

int
jack_process_cb(
  jack_nframes_t nframes,
  void * context_ptr)
{
  float * left;
  float * right;
  void * input_buf;
  jack_midi_event_t input_event;
  jack_nframes_t input_event_count;
  jack_nframes_t i;
  jack_nframes_t offset;

  input_buf = jack_port_get_buffer(fluidjack_ptr->in, nframes);
  input_event_count = jack_midi_get_event_count(input_buf);

  left = (float *)jack_port_get_buffer(fluidjack_ptr->left, nframes);
  right = (float *)jack_port_get_buffer(fluidjack_ptr->right, nframes);

  offset = 0;
  for (i = 0; i < input_event_count; i++)
  {
    /* retrieve JACK MIDI event */
    jack_midi_event_get(&input_event, input_buf, i);

    assert(input_event.time >= offset);
    if (input_event.time > offset)
    {
      /* generate audio up to event */
      fluid_synth_write_float(fluidjack_ptr->synth, input_event.time - offset, left + offset, 0, 1, right + offset, 0, 1);

      offset = input_event.time;
    }

    if (input_event.size == 3)
    {
      if ((input_event.buffer[0] >> 4) == 0x08)
      {                         /* note off */
        //LOG_NOTICE("note off");
        fluid_synth_noteoff(fluidjack_ptr->synth, input_event.buffer[0] & 0x0F, input_event.buffer[1]);
      }
      else if ((input_event.buffer[0] >> 4) == 0x09)
      {                         /* note on */
        //LOG_NOTICE("note on");
        fluid_synth_noteon(fluidjack_ptr->synth, input_event.buffer[0] & 0x0F, input_event.buffer[1], input_event.buffer[2]);
      }
      else if ((input_event.buffer[0] >> 4) == 0x0B)
      {
        fluid_synth_cc(fluidjack_ptr->synth, input_event.buffer[0] & 0x0F, input_event.buffer[1], input_event.buffer[2]);
      }
      else if ((input_event.buffer[0] >> 4) == 0x0E)
      {
        fluid_synth_pitch_bend(fluidjack_ptr->synth, input_event.buffer[0] & 0x0F, (input_event.buffer[2] << 7) | input_event.buffer[1]);
      }
    }
    if (input_event.size == 2)
    {
      if ((input_event.buffer[0] >> 4) == 0x0C)
      {
        fluid_synth_program_change(fluidjack_ptr->synth, input_event.buffer[0] & 0x0F, input_event.buffer[1]);
      }
    }
  }

  if (nframes > offset)
  {
    fluid_synth_write_float(fluidjack_ptr->synth, nframes - offset, left + offset, 0, 1, right + offset, 0, 1);
  }

  return 0;
}

#undef fluidjack_ptr

int unblock = 0;    // (write-end of the pipe)
int block_check = 0;  // (read-end of the pipe)

bool wait_input(void)
{
  fd_set read_set;
  int unblock_pipe[2];

  if (!unblock)
  {
    // no block/unblock pipe yet....
    if (pipe(unblock_pipe))
    {
      return false;
    }
    unblock = unblock_pipe[1];    // unblock key
    block_check = unblock_pipe[0];  // block check
  }

  // find out if (input_is_ready_on_stdin || unblock_key_has_been_activated)
  FD_ZERO(&read_set);
  FD_SET(0,&read_set);
  FD_SET(block_check,&read_set);  // if there's anything to read on the pipe unblock_key has been activated!

  // wait until input is ready or a cancel wait was issued
  if (select(block_check+1,&read_set,NULL,NULL,NULL) == -1)
  {
    return false;
  }

  if (FD_ISSET(0,&read_set))
  {
    return true;      // input ready on stdin
  }
  else
  {
    if (block_check)    // user issued cancel wait!
    {
      close(block_check);
    }
    block_check = 0;
    return false;
  }

}

void wait_input_cancel(void)
{
  if (unblock)
  {
    write(unblock,'\0',1);  // stub!
    close(unblock);
    unblock = 0;
  }
}

void signal_handler(int signum)
{
  LOG_NOTICE("Caught signal %d (%s), terminating", signum, strsignal(signum));
  wait_input_cancel();
}

int main(int argc, char ** argv)
{
  int ret;
  struct fluidjack fluidjack;
  jack_nframes_t sample_rate;

  if (argc != 2)
  {
    LOG_ERROR("Usage: fluidjack <path_to_sf2_file>");
    ret = 1;
    goto exit;
  }

  /* Connect to JACK (with plugin name as client name) */
  fluidjack.jack_client = jack_client_open("fluidjack", JackNullOption, NULL);
  if (fluidjack.jack_client == NULL)
  {
    LOG_ERROR("Failed to connect to JACK.");
    ret = 1;
    goto exit;
  }

  fluidjack.settings = new_fluid_settings();
  if (fluidjack.settings == NULL)
  {
    LOG_ERROR("new_fluid_settings() failed.");
    ret = 1;
    goto exit_close_jack_client;
  }

  sample_rate = jack_get_sample_rate(fluidjack.jack_client);
  LOG_NOTICE("Sample rate is %u", (unsigned int)sample_rate);
  fluid_settings_setnum(fluidjack.settings, "synth.sample-rate", sample_rate);

  fluidjack.synth = new_fluid_synth(fluidjack.settings);
  if (fluidjack.synth == NULL)
  {
    LOG_ERROR("new_fluid_synth() failed.");
    ret = 1;
    goto exit_delete_settings;
  }

  LOG_NOTICE("Loading soundfont \"%s\"...", argv[1]);
  fluidjack.sf_id = fluid_synth_sfload(fluidjack.synth, argv[1], 1);
  if (fluidjack.sf_id == -1)
  {
    LOG_ERROR("Soundfont loading failed.");
    ret = 1;
    goto exit_delete_synth;
  }

  ret = jack_set_process_callback(fluidjack.jack_client, &jack_process_cb, &fluidjack);
  if (ret != 0)
  {
    LOG_ERROR("jack_set_process_callback() failed.");
    ret = 1;
    goto exit_delete_synth;
  }

  fluidjack.in = jack_port_register(fluidjack.jack_client, "in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
  if (fluidjack.in == NULL)
  {
    LOG_ERROR("Failed to register JACK MIDI input port.");
    ret = 1;
    goto exit_delete_synth;
  }

  fluidjack.left = jack_port_register(fluidjack.jack_client, "left", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  if (fluidjack.left == NULL)
  {
    LOG_ERROR("Failed to register JACK audio output left port.");
    ret = 1;
    goto exit_delete_synth;
  }

  fluidjack.right = jack_port_register(fluidjack.jack_client, "right", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  if (fluidjack.right == NULL)
  {
    LOG_ERROR("Failed to register JACK audio output right port.");
    ret = 1;
    goto exit_delete_synth;
  }

  ret = jack_activate(fluidjack.jack_client);
  if (ret != 0)
  {
    LOG_ERROR("jack_activate() failed.");
    ret = 1;
    goto exit_delete_synth;
  }

  LOG_NOTICE("Press Enter key to exit");

  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);

  wait_input();

  jack_deactivate(fluidjack.jack_client);

exit_delete_synth:
  delete_fluid_synth(fluidjack.synth);

exit_delete_settings:
  delete_fluid_settings(fluidjack.settings);

exit_close_jack_client:
  LOG_NOTICE("closing jack client");
  jack_client_close(fluidjack.jack_client);

exit:
  return ret;
}
