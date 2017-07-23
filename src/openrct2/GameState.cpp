#pragma region Copyright (c) 2014-2017 OpenRCT2 Developers
/*****************************************************************************
* OpenRCT2, an open source clone of Roller Coaster Tycoon 2.
*
* OpenRCT2 is the work of many authors, a full list can be found in contributors.md
* For more information, visit https://github.com/OpenRCT2/OpenRCT2
*
* OpenRCT2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* A full copy of the GNU General Public License can be found in licence.txt
*****************************************************************************/
#pragma endregion

#include "GameState.h"
#include "Context.h"
#include "core/Math.hpp"
#include "Editor.h"
#include "Input.h"
#include "interface/Screenshot.h"
#include "management/NewsItem.h"
#include "network/network.h"
#include "OpenRCT2.h"
#include "platform/Platform2.h"
#include "scenario/Scenario.h"
#include "title/TitleScreen.h"
#include "title/TitleSequencePlayer.h"
#include "world/Climate.h"
#include "world/MapAnimation.h"
#include "world/Park.h"

using namespace OpenRCT2;

GameState::GameState()
{
    _park = std::make_unique<Park>();
}

void GameState::Update()
{
    gInUpdateCode = true;

    uint32 numUpdates;

    // 0x006E3AEC // screen_game_process_mouse_input();
    screenshot_check();
    game_handle_keyboard_input();

    if (game_is_not_paused() && gPreviewingTitleSequenceInGame)
    {
        title_sequence_player_update((ITitleSequencePlayer *) title_get_sequence_player());
    }

    // Determine how many times we need to update the game
    if (gGameSpeed > 1)
    {
        numUpdates = 1 << (gGameSpeed - 1);
    }
    else
    {
        numUpdates = gTicksSinceLastUpdate / GAME_UPDATE_TIME_MS;
        numUpdates = Math::Clamp<uint32>(1, numUpdates, GAME_MAX_UPDATES);
    }

    if (network_get_mode() == NETWORK_MODE_CLIENT && network_get_status() == NETWORK_STATUS_CONNECTED && network_get_authstatus() == NETWORK_AUTH_OK)
    {
        if (network_get_server_tick() - gCurrentTicks >= 10)
        {
            // Make sure client doesn't fall behind the server too much
            numUpdates += 10;
        }
    }

    if (game_is_paused())
    {
        numUpdates = 0;
        // Update the animation list. Note this does not
        // increment the map animation.
        map_animation_invalidate_all();

        // Special case because we set numUpdates to 0, otherwise in game_logic_update.
        network_update();

        network_process_game_commands();
    }

    // Update the game one or more times
    for (uint32 i = 0; i < numUpdates; i++)
    {
        UpdateLogic();
        if (gGameSpeed == 1)
        {
            if (input_get_state() == INPUT_STATE_RESET ||
                input_get_state() == INPUT_STATE_NORMAL)
            {
                if (input_test_flag(INPUT_FLAG_VIEWPORT_SCROLLING))
                {
                    input_set_flag(INPUT_FLAG_VIEWPORT_SCROLLING, false);
                    break;
                }
            }
            else
            {
                break;
            }
        }
    }

    if (!gOpenRCT2Headless)
    {
        input_set_flag(INPUT_FLAG_VIEWPORT_SCROLLING, false);

        // the flickering frequency is reduced by 4, compared to the original
        // it was done due to inability to reproduce original frequency
        // and decision that the original one looks too fast
        if (gCurrentTicks % 4 == 0)
            gWindowMapFlashingFlags ^= (1 << 15);

        // Handle guest map flashing
        gWindowMapFlashingFlags &= ~(1 << 1);
        if (gWindowMapFlashingFlags & (1 << 0))
            gWindowMapFlashingFlags |= (1 << 1);
        gWindowMapFlashingFlags &= ~(1 << 0);

        // Handle staff map flashing
        gWindowMapFlashingFlags &= ~(1 << 3);
        if (gWindowMapFlashingFlags & (1 << 2))
            gWindowMapFlashingFlags |= (1 << 3);
        gWindowMapFlashingFlags &= ~(1 << 2);

        context_update_map_tooltip();

        // Input
        gUnk141F568 = gUnk13CA740;

        context_handle_input();
    }

    // Always perform autosave check, even when paused
    if (!(gScreenFlags & SCREEN_FLAGS_TITLE_DEMO) &&
        !(gScreenFlags & SCREEN_FLAGS_TRACK_DESIGNER) &&
        !(gScreenFlags & SCREEN_FLAGS_TRACK_MANAGER))
    {
        scenario_autosave_check();
    }

    window_dispatch_update_all();

    gGameCommandNestLevel = 0;
    gInUpdateCode         = false;
}

void GameState::UpdateLogic()
{
    gScreenAge++;
    if (gScreenAge == 0)
        gScreenAge--;

    network_update();

    if (network_get_mode() == NETWORK_MODE_CLIENT &&
        network_get_status() == NETWORK_STATUS_CONNECTED &&
        network_get_authstatus() == NETWORK_AUTH_OK)
    {
        // Can't be in sync with server, round trips won't work if we are at same level.
        if (gCurrentTicks >= network_get_server_tick())
        {
            // Don't run past the server
            return;
        }
    }

    if (network_get_mode() == NETWORK_MODE_SERVER)
    {
        // Send current tick out.
        network_send_tick();
    }
    else if (network_get_mode() == NETWORK_MODE_CLIENT)
    {
        // Check desync.
        network_check_desynchronization();
    }

    sub_68B089();
    scenario_update();
    climate_update();
    map_update_tiles();
    // Temporarily remove provisional paths to prevent peep from interacting with them
    map_remove_provisional_elements();
    map_update_path_wide_flags();
    peep_update_all();
    map_restore_provisional_elements();
    vehicle_update_all();
    sprite_misc_update_all();
    ride_update_all();

    if (!(gScreenFlags & (SCREEN_FLAGS_SCENARIO_EDITOR | SCREEN_FLAGS_TRACK_DESIGNER | SCREEN_FLAGS_TRACK_MANAGER)))
    {
        _park->Update();
    }

    research_update();
    ride_ratings_update_all();
    ride_measurements_update();
    news_item_update_current();

    map_animation_invalidate_all();
    vehicle_sounds_update();
    peep_update_crowd_noise();
    climate_update_sound();
    editor_open_windows_for_current_step();

    // Update windows
    //window_dispatch_update_all();

    if (gErrorType != ERROR_TYPE_NONE)
    {
        rct_string_id title_text = STR_UNABLE_TO_LOAD_FILE;
        rct_string_id body_text  = gErrorStringId;
        if (gErrorType == ERROR_TYPE_GENERIC)
        {
            title_text = gErrorStringId;
            body_text  = 0xFFFF;
        }
        gErrorType = ERROR_TYPE_NONE;

        context_show_error(title_text, body_text);
    }

    // Start autosave timer after update
    if (gLastAutoSaveUpdate == AUTOSAVE_PAUSE)
    {
        gLastAutoSaveUpdate = Platform::GetTicks();
    }

    // Separated out processing commands in network_update which could call scenario_rand where gInUpdateCode is false.
    // All commands that are received are first queued and then executed where gInUpdateCode is set to true.
    network_process_game_commands();

    network_flush();

    gCurrentTicks++;
    gScenarioTicks++;
    gSavedAge++;
}
