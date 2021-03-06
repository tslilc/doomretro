﻿/*
========================================================================

                           D O O M  R e t r o
         The classic, refined DOOM source port. For Windows PC.

========================================================================

  Copyright © 1993-2012 by id Software LLC, a ZeniMax Media company.
  Copyright © 2013-2020 by Brad Harding.

  DOOM Retro is a fork of Chocolate DOOM. For a list of credits, see
  <https://github.com/bradharding/doomretro/wiki/CREDITS>.

  This file is a part of DOOM Retro.

  DOOM Retro is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation, either version 3 of the License, or (at your
  option) any later version.

  DOOM Retro is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with DOOM Retro. If not, see <https://www.gnu.org/licenses/>.

  DOOM is a registered trademark of id Software LLC, a ZeniMax Media
  company, in the US and/or other countries, and is used without
  permission. All other trademarks are the property of their respective
  holders. DOOM Retro is in no way affiliated with nor endorsed by
  id Software.

========================================================================
*/

#include <stdlib.h>
#include <windows.h>
#include <Psapi.h>
#include <thread>
#include <vector>

#include "SDL.h"
#include "SDL_mixer.h"

#include "midiproc.h"
#include "..\src\version.h"

#pragma comment(lib, "psapi.lib")

// Currently playing music track
static Mix_Music    *music;
static SDL_RWops    *rw;

static void UnregisterSong(void);

//
// Sentinel that checks doomretro.exe is actually running
//
class AutoHandle
{
public:
    HANDLE  handle;

    AutoHandle(HANDLE h) : handle(h) {}

    ~AutoHandle()
    {
        if (handle != nullptr)
            CloseHandle(handle);
    }
};

static boolean Sentinel_EnumerateProcesses(std::vector<DWORD> &ndwPIDs, size_t &numValidPIDs)
{
    while (1)
    {
        DWORD   cb = static_cast<DWORD>(ndwPIDs.size() * sizeof(DWORD));
        DWORD   cbNeeded = 0;

        if (!EnumProcesses(&ndwPIDs[0], cb, &cbNeeded))
            return false;
        if (cb == cbNeeded)
            // try again with a larger array
            ndwPIDs.resize(ndwPIDs.size() * 2);
        else
        {
            // successful
            numValidPIDs = cbNeeded / sizeof(DWORD);
            return true;
        }
    }
}

static boolean Sentinel_FindPID(const std::vector<DWORD> &ndwPIDs, HANDLE &pHandle, size_t numValidPIDs)
{
    for (size_t i = 0; i < numValidPIDs; i++)
    {
        AutoHandle  chProcess(OpenProcess((PROCESS_QUERY_INFORMATION | PROCESS_VM_READ), FALSE, ndwPIDs[i]));

        if (chProcess.handle != nullptr)
        {
            char    szProcessImage[MAX_PATH];

            ZeroMemory(szProcessImage, sizeof(szProcessImage));

            if (GetProcessImageFileNameA(chProcess.handle, szProcessImage, sizeof(szProcessImage)))
            {
                const size_t    imageLength = strlen(szProcessImage);
                const size_t    filenameLength = strlen(PACKAGE_FILENAME);

                if (imageLength < filenameLength)
                    continue;

                // Lop off the start of szProcessImage
                if (!_strnicmp(szProcessImage + imageLength - filenameLength, PACKAGE_FILENAME, filenameLength))
                {
                    pHandle = chProcess.handle;
                    chProcess.handle = nullptr;     // Abuse AutoHandle's destructor behavior
                    return true;
                }
            }
        }
    }

    return false;
}

void Sentinel_Main()
{
    constexpr size_t    initMaxNumPIDs = 1024;
    std::vector<DWORD>  ndwPIDs(initMaxNumPIDs, 0);
    HANDLE              pHandle;
    size_t              numValidPIDs;
    DWORD               dwExitCode;

    if (!Sentinel_EnumerateProcesses(ndwPIDs, numValidPIDs))
        exit(-1);

    if (!Sentinel_FindPID(ndwPIDs, pHandle, numValidPIDs))
    {
        MessageBox(NULL, TEXT(PACKAGE_FILENAME " is not running."), TEXT("midiproc.exe"), MB_ICONERROR);
        exit(-1);
    }

    do
    {
        if (!GetExitCodeProcess(pHandle, &dwExitCode))
            exit(-1);

        Sleep(100);
    } while (dwExitCode == STILL_ACTIVE);

    exit(-1);
}

//
// RPC Memory Management
//
void __RPC_FAR * __RPC_USER midl_user_allocate(size_t size)
{
    return malloc(size);
}

void __RPC_USER midl_user_free(void __RPC_FAR *p)
{
    free(p);
}

//
// SDL_mixer Interface
//

//
// InitSDL
// Start up SDL and SDL_mixer.
//
static boolean InitSDL(void)
{
    if (SDL_Init(SDL_INIT_AUDIO) == -1)
        return false;

    if (Mix_OpenAudioDevice(44100, MIX_DEFAULT_FORMAT, 2, 1024, NULL, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE) < 0)
        return false;

    return true;
}

//
// RegisterSong
//
static void RegisterSong(void *data, size_t size)
{
    if (music)
        UnregisterSong();

    rw = SDL_RWFromMem(data, (int)size);
    music = Mix_LoadMUS_RW(rw, SDL_FALSE);
}

//
// StartSong
//
static void StartSong(boolean loop)
{
    if (music)
        Mix_PlayMusic(music, (loop ? -1 : 0));
}

//
// SetVolume
//
static void SetVolume(int volume)
{
    Mix_VolumeMusic(volume);
}

static int  paused_midi_volume;

//
// PauseSong
//
static void PauseSong(void)
{
    paused_midi_volume = Mix_VolumeMusic(-1);
    Mix_VolumeMusic(0);
}

//
// ResumeSong
//
static void ResumeSong(void)
{
    Mix_VolumeMusic(paused_midi_volume);
}

//
// StopSong
//
static void StopSong(void)
{
    if (music)
        Mix_HaltMusic();
}

//
// UnregisterSong
//
static void UnregisterSong(void)
{
    if (!music)
        return;

    StopSong();
    Mix_FreeMusic(music);
    rw = NULL;
    music = NULL;
}

//
// ShutdownSDL
//
static void ShutdownSDL(void)
{
    if (music)
    {
        Mix_FadeOutMusic(500);
        while (Mix_PlayingMusic());
        UnregisterSong();
        Mix_FreeMusic(music);
        rw = NULL;
        music = NULL;
    }

    Mix_CloseAudio();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

//
// Song Buffer
//
// The MIDI program will be transmitted by the client across RPC in fixed-size
// chunks until all data has been transmitted.
//
typedef unsigned char   midibyte;

class SongBuffer
{
protected:
    midibyte            *buffer;                    // accumulated input
    size_t              size;                       // size of input
    size_t              allocated;                  // amount of memory allocated (>= size)

    static const int    defaultSize = 128 * 1024;   // 128 KB

public:
    // Constructor
    // Start out with an empty 128 KB buffer.
    SongBuffer()
    {
        buffer = static_cast<midibyte *>(calloc(1, defaultSize));
        size = 0;
        allocated = defaultSize;
    }

    // Destructor.
    // Release the buffer.
    ~SongBuffer()
    {
        if (buffer)
        {
            free(buffer);
            buffer = NULL;
            size = 0;
            allocated = 0;
        }
    }

    //
    // addChunk
    //
    // Add a chunk of MIDI data to the buffer.
    //
    void addChunk(midibyte *data, size_t newsize)
    {
        if (size + newsize > allocated)
        {
            allocated += newsize * 2;
            buffer = static_cast<midibyte *>(realloc(buffer, allocated));
        }

        memcpy(buffer + size, data, newsize);
        size += newsize;
    }

    // Accessors
    midibyte *getBuffer() const { return buffer; }
    size_t    getSize()   const { return size;   }
};

static SongBuffer   *song;

//
// RPC Server Interface
//

//
// MidiRPC_PrepareNewSong
//
// Prepare the engine to receive new song data from the RPC client.
//
void MidiRPC_PrepareNewSong(void)
{
    // Stop anything currently playing and free it.
    UnregisterSong();

    // free any previous song buffer
    delete song;

    // prep new song buffer
    song = new SongBuffer();
}

//
// MidiRPC_AddChunk
//
// Add a chunk of data to the song.
//
void MidiRPC_AddChunk(unsigned int count, byte *pBuf)
{
    song->addChunk(pBuf, static_cast<size_t>(count));
}

//
// MidiRPC_PlaySong
//
// Start playing the song.
//
void MidiRPC_PlaySong(boolean looping)
{
    RegisterSong(song->getBuffer(), song->getSize());
    StartSong(looping);
}

//
// MidiRPC_StopSong
//
// Stop the song.
//
void MidiRPC_StopSong(void)
{
    StopSong();
}

//
// MidiRPC_ChangeVolume
//
// Set playback volume level.
//
void MidiRPC_ChangeVolume(int volume)
{
    SetVolume(volume);
}

//
// MidiRPC_PauseSong
//
// Pause the song.
//
void MidiRPC_PauseSong(void)
{
    PauseSong();
}

//
// MidiRPC_ResumeSong
//
// Resume after pausing.
//
void MidiRPC_ResumeSong(void)
{
    ResumeSong();
}

//
// MidiRPC_StopServer
//
// Stops the RPC server so the program can shutdown.
//
void MidiRPC_StopServer(void)
{
    // Local shutdown tasks
    ShutdownSDL();
    delete song;
    song = NULL;

    // Stop RPC server
    RpcMgmtStopServerListening(NULL);
}

//
// RPC Server Init
//
static boolean MidiRPC_InitServer(void)
{
    RPC_STATUS  status;

    // Initialize RPC protocol
    status = RpcServerUseProtseqEp((RPC_CSTR)"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        (RPC_CSTR)"2d4dc2f9-ce90-4080-8a00-1cb819086970", NULL);

    if (status)
        return false;

    // Register server
    status = RpcServerRegisterIf(MidiRPC_v1_0_s_ifspec, NULL, NULL);

    if (status)
        return false;

    // Start listening
    status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, FALSE);

    return !status;
}

//
// Main Program
//

//
// WinMain
//
// Application entry point.
//
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // Initialize SDL
    if (!InitSDL())
        return -1;

    std::thread watcher(Sentinel_Main);

    // Initialize RPC Server
    if (!MidiRPC_InitServer())
        return -1;

    return 0;
}
