__start = 0x80000000;

/* Dummy addresses that get recompiled into function calls */
recomp_puts = 0x8F000000;
recomp_exit = 0x8F000004;
recomp_load_overlays = 0x8F000008;
osInvalDCache_recomp = 0x8F00000C;
osInvalICache_recomp = 0x8F000010;
osWritebackDCache_recomp = 0x8F000014;
osWritebackDCacheAll_recomp = 0x8F000018;
bzero_recomp = 0x8F00001C;
recomp_load_overlays_by_rom = 0x8F000020;
osSendMesg_recomp = 0x8F000024;
osGetCount_recomp = 0x8F000028;
osCreateMesgQueue_recomp = 0x8F00002C;
osRecvMesg_recomp = 0x8F000030;
osDpSetStatus_recomp = 0x8F000034;
malloc_recomp = 0x8F000038;
free_recomp = 0x8F00003C;
realloc_recomp = 0x8F000040;
recomp_register_object_extension = 0x8F000044;
recomp_register_object_extension_generic = 0x8F000048;
recomp_clear_all_object_data = 0x8F00004C;
recomp_create_object_data = 0x8F000050;
recomp_destroy_object_data = 0x8F000054;
recomp_get_object_data = 0x8F000058;
recomp_get_object_spawn_index = 0x8F00005C;
recomp_error = 0x8F000060;
memcpy_recomp = 0x8F000064;
__divdi3_recomp = 0x8F000068;
__udivdi3_recomp = 0x8F00006C;
__umoddi3_recomp = 0x8F000070;
strlen_recomp = 0x8F000074;
osVirtualToPhysical_recomp = 0x8F000078;
osPiStartDma_recomp = 0x8F00007C;
recomp_abort = 0x8F000080;
recomp_get_target_aspect_ratio = 0x8F000084;
osExQueueDisplaylistEvent_recomp = 0x8F000088;
recomp_xxh3 = 0x8F00008C;
recomp_get_bgm_volume = 0x8F000090;
sinf_recomp = 0x8F000094;
__cosf_recomp = 0x8F000098;
recomp_get_right_analog_inputs = 0x8F00009C;
recomp_get_analog_cam_enabled = 0x8F0000A0;
recomp_get_analog_inverted_axes = 0x8F0000A4;
recomp_set_right_analog_suppressed = 0x8F0000A8;
osContGetReadData_recomp = 0x8F0000AC;
bcopy_recomp = 0x8F0000B0;
recomp_get_note_saving_enabled = 0x8F0000B4;
recomp_get_cutscene_aspect_ratio = 0x8F0000B8;
recomp_get_flying_and_swimming_inverted_axes = 0x8F0000BC;
recomp_get_first_person_inverted_axes = 0x8F0000C0;
recomp_get_analog_cam_sensitivity = 0x8F0000C4;
recomp_run_ui_callbacks = 0x8F0000C8;

/* Networking functions */
recomp_net_push_full_state = 0x8F0000CC;
recomp_net_push_local_state = 0x8F0000D0;
recomp_net_is_connected = 0x8F0000D4;
recomp_net_get_remote_state = 0x8F0000D8;
recomp_net_get_remote_count = 0x8F0000DC;
recomp_net_get_local_player_id = 0x8F0000E0;

/* World state sync functions */
recomp_net_send_collectible = 0x8F0000E4;
recomp_net_send_enemy_death = 0x8F0000E8;
recomp_net_send_flag_change = 0x8F0000EC;
recomp_net_pop_world_event = 0x8F0000F0;

/* Enemy position sync */
recomp_net_is_host = 0x8F0000F4;
recomp_net_send_enemy_positions = 0x8F0000F8;
recomp_net_get_enemy_positions = 0x8F0000FC;

/* Full state sync */
recomp_net_should_send_full_sync = 0x8F000100;
recomp_net_send_world_state_full = 0x8F000104;
recomp_net_pop_full_state = 0x8F000108;

/* World ownership */
recomp_net_am_i_world_owner = 0x8F00010C;
recomp_net_send_owner_transfer = 0x8F000110;
recomp_net_pop_owner_transfer = 0x8F000114;
recomp_net_push_level_id = 0x8F000118;
