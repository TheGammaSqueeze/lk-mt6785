require("platform.lua")
require("/scripts/env/env_common.lua")
require("/scripts/DecorativeFrames.lua")
require("/scripts/app/clover_const.lua")

system = class()
system.hud = {}
system.dialogs = {}
system.loadingCount = 0
system.loadingCount_thumbnail = 0
system.platform = CLOVER_UI_PLATFORM
system.wait_boot_fade = {}
system.is_roll_back_return = false
system.is_roll_back_cancel = false
STARFOX_THE_END_FILENAME = "/var/lib/clover/profiles/0/StarFox-TheEnd.txt"

local var_0_0 = 128

function system.init_title()
	print("init_title")

	if REED_DEBUG and titles_list then
		titles_list = GameList:limitTitleNum(titles_list)
	end

	GameList:loadThumbnail()
end

function system.load_setting()
	print("load_setting")
	Store:init()
	Store:load()

	if not store.info then
		store.info = {}
	end
end

function system.save_setting()
	print("save_setting")

	if REED_DEBUG and (debug_store.initSave and false or debug_store.initDebugsave) then
		Store:delete_debugsave()
	end

	Store:save()
end

function system.renamefile(arg_4_0, arg_4_1)
	local var_4_0 = FileUtils.cleanupPath(arg_4_0)
	local var_4_1 = FileUtils.cleanupPath(arg_4_1)
	local var_4_2

	if not FileUtils.copyFile(var_4_0, var_4_1) then
		print("copy failed")

		return
	end

	if not FileUtils.deleteFile(var_4_0) then
		print("delete failed")

		return
	end

	return true
end

function system.move_savecontents_callback(arg_5_0)
	if not arg_5_0 then
		print("system.move_savecontents failed!" .. tostring(arg_5_0))
	end
end

function system.move_savecontents(arg_6_0, arg_6_1, arg_6_2)
	local var_6_0 = FileUtils.cleanupPath(arg_6_0)
	local var_6_1 = FileUtils.cleanupPath(arg_6_1)
	local var_6_2 = string.match(var_6_1, ".*/")

	print("move_savecontents")

	if debug_store.force_fileerror then
		error("force_fileerror")
	end

	if HOST_PLATFORM_IS_WINDOWS then
		return var_6_0
	end

	if arg_6_2 then
		local var_6_3 = coroutine.running()

		assert(var_6_3)
		SaveContentHelper:MoveDirectoryAsync(var_6_0, var_6_1, function(arg_7_0)
			if not arg_7_0 then
				SaveContentHelper:EnsureConsistency(var_6_2)
			end

			coroutine.resume(var_6_3, arg_7_0)
		end)
	elseif not SaveContentHelper:MoveDirectory(var_6_0, var_6_1) then
		SaveContentHelper:EnsureConsistency(var_6_2)
	end

	return var_6_1
end

function system.move_resumedata(arg_8_0, arg_8_1, arg_8_2, arg_8_3)
	local var_8_0 = PERSISTENT_SUSPENSION_POINTS_PATH .. arg_8_1 .. "/suspendpoint" .. tostring(arg_8_2)

	return system.move_savecontents(arg_8_0, var_8_0, arg_8_3)
end

function system.rename_resumedata(arg_9_0, arg_9_1, arg_9_2)
	local var_9_0 = FileUtils.cleanupPath(arg_9_0)

	print("move_resumedata")

	if debug_store.force_fileerror then
		error("force_fileerror")
	end

	if HOST_PLATFORM_IS_WINDOWS then
		return var_9_0
	end

	local var_9_1 = PERSISTENT_SUSPENSION_POINTS_PATH .. arg_9_1 .. "/suspendpoint" .. tostring(arg_9_2)

	SaveContentHelper:RenameDirectory(var_9_0, var_9_1)

	return var_9_1
end

function system.delete_resumedata(arg_10_0)
	local var_10_0 = FileUtils.cleanupPath(arg_10_0)

	print("delete_resumedata")

	if debug_store.force_fileerror then
		error("force_fileerror")
	end

	if HOST_PLATFORM_IS_WINDOWS then
		return
	end

	if FileUtils.directoryExists(var_10_0) then
		local var_10_1, var_10_2 = FileUtils.deleteDirectory(var_10_0)

		if not var_10_1 then
			debugLabelPrint(var_10_2 or "")
			error(var_10_2)
		end
	end
end

function system.swap_resumedata(arg_11_0, arg_11_1, arg_11_2)
	local var_11_0 = "suspendpoint" .. tostring(arg_11_1)
	local var_11_1 = "suspendpoint" .. tostring(arg_11_2)

	print("swap_resumedata")

	if not SaveContentHelper:SwapDirectories(PERSISTENT_SUSPENSION_POINTS_PATH .. arg_11_0, var_11_0, var_11_1) then
		SaveContentHelper:EnsureConsistency(PERSISTENT_SUSPENSION_POINTS_PATH .. "/" .. arg_11_0 .. "/")
		error()

		return false
	end

	return true
end

function system.initialize()
	print("initialize")
	Store:delete()

	system.noSave = true

	system.fadeOut(mcp.reset_to_factory)
end

function system.run_gamestart(arg_13_0, arg_13_1)
	system.play_count(arg_13_0)

	local var_13_0 = false

	if system.floating_save and system.floating_cards then
		system.floating_cards:erase()
	end

	debugLabelPrint("GAME START : MCP Version")

	local var_13_1 = VOLATILE_SUSPENSION_POINTS_PATH .. arg_13_0 .. "/suspendpoint0/rollback/"

	if arg_13_1 == nil then
		arg_13_1 = titles_list[arg_13_0].sram_file
	end

	local var_13_2
	local var_13_3 = VOLATILE_SUSPENSION_POINTS_PATH .. arg_13_0 .. "/suspendpoint0/state.time"
	local var_13_4 = VOLATILE_SUSPENSION_POINTS_PATH .. arg_13_0 .. "/suspendpoint0/state.png"
	local var_13_5 = system.make_display_option_table()

	if HOST_PLATFORM_IS_WINDOWS then
		system.fadeOut()

		return
	end

	local var_13_6 = {}
	local var_13_7 = system.make_frame_option_table()

	system.clearBootRollback()
	system.fadeOut(mcp2.launch_title, arg_13_0, var_13_1, var_13_5, arg_13_1, var_13_2, var_13_3, var_13_4, var_13_7)
	Main:stopMainBGM()
end

function system.run_gameresume(arg_14_0, arg_14_1, arg_14_2, arg_14_3, arg_14_4, arg_14_5, arg_14_6)
	local var_14_0 = FileUtils.cleanupPath(arg_14_1)

	print(string.format("run_gameresume / roll_back:%s", tostring(arg_14_2)), var_14_0 or "!!nil")
	system.play_count(arg_14_0)

	if arg_14_2 == false and system.floating_save and system.floating_cards and not arg_14_4 then
		system.floating_cards:erase()
	end

	if not arg_14_3 then
		store.nextDelete = var_14_0
	end

	store.last_suspentionpoint = var_14_0

	debugLabelPrint("GAME START : MCP Version")

	local var_14_1 = var_14_0
	local var_14_2 = VOLATILE_SUSPENSION_POINTS_PATH .. arg_14_0 .. "/suspendpoint0"

	if arg_14_2 then
		var_14_2 = VOLATILE_SUSPENSION_POINTS_PATH .. "rollback"
	end

	local var_14_3 = var_14_2 .. "/rollback/"

	if arg_14_6 == nil then
		arg_14_6 = titles_list[arg_14_0].sram_file
	end

	local var_14_4 = var_14_2 .. "/state.time"
	local var_14_5 = var_14_2 .. "/state.png"
	local var_14_6 = system.make_display_option_table()

	if HOST_PLATFORM_IS_WINDOWS then
		system.fadeOut()

		return
	end

	local var_14_7 = {}
	local var_14_8 = system.make_frame_option_table()

	if arg_14_2 then
		system.setBootRollback(arg_14_0, arg_14_4 == true)
		system.setFadeType(CloverConst.Fade.ROLLBACK_FADE_TYPE)
		system.fadeOut(mcp2.resume_title_replay, arg_14_0, var_14_1, var_14_3, var_14_6, arg_14_6, arg_14_5, var_14_4, var_14_5, var_14_8)
	else
		system.clearBootRollback()
		mcp2.resume_title_record(arg_14_0, var_14_1, var_14_3, var_14_6, arg_14_6, arg_14_5, var_14_4, var_14_5, var_14_8)
		system.fadeOut(Application.exit)
	end

	Main:stopMainBGM()
end

function system.run_gamedemoplay(arg_15_0)
	print("run_gamedemoplay")

	local var_15_0 = titles_list[arg_15_0]

	if HOST_PLATFORM_IS_WINDOWS then
		return
	end

	debugLabelPrint("GAME START : MCP Version")

	local var_15_1 = ""
	local var_15_2 = ""

	if not table_isEmpty(var_15_0.autoplay) then
		local var_15_3 = #var_15_0.autoplay

		if not store.info then
			store.info = {}
		end

		if not store.info[arg_15_0] then
			store.info[arg_15_0] = {}
		end

		if not store.info[arg_15_0].demo_index then
			store.info[arg_15_0].demo_index = 1
		end

		local var_15_4 = store.info[arg_15_0].demo_index

		store.info[arg_15_0].demo_index = index_loop_next(store.info[arg_15_0].demo_index, var_15_3)
		var_15_1 = var_15_0.autoplay[var_15_4] .. ".state"
		var_15_2 = var_15_0.autoplay[var_15_4]
	elseif CLOVER_IS_DEBUG then
		arg_15_0 = "CLV-P-VAAAJ"
		var_15_1 = Application.getDataPath("dummy/dummy.inputs") .. ".state"
		var_15_2 = Application.getDataPath("dummy/dummy.inputs")
	else
		return
	end

	if CLOVER_IS_DEBUG and debug_store.debugDemoShort then
		var_15_2 = Application.getDataPath("dummy/dummy.inputs")
	end

	local var_15_5 = system.make_display_option_table()
	local var_15_6 = {}
	local var_15_7 = system.make_frame_option_table()

	system.fadeOut(mcp.start_super_play, arg_15_0, var_15_1, var_15_2, var_15_5, var_15_7)
end

function system.run_gamemyplay(arg_16_0, arg_16_1, arg_16_2)
	print("run_gamemyplay")

	if HOST_PLATFORM_IS_WINDOWS then
		return
	end

	debugLabelPrint("GAME START : MCP Version")

	local var_16_0 = 0

	if arg_16_2 then
		local var_16_1 = titles_list[arg_16_0].myplay_demo_time

		if var_16_1 then
			var_16_0 = system.get_myplay_start_index(arg_16_1, var_16_1)
		end
	end

	local var_16_2 = arg_16_1 .. var_16_0 .. ".break"
	local var_16_3 = arg_16_1 .. var_16_0 .. ".inputs"
	local var_16_4 = system.make_display_option_table()
	local var_16_5 = {}
	local var_16_6 = system.make_frame_option_table()

	system.fadeOut(mcp.start_super_play, arg_16_0, var_16_2, var_16_3, var_16_4, var_16_6)
end

function system.get_myplay_start_index(arg_17_0, arg_17_1)
	local var_17_0 = system.get_rollback_durations(arg_17_0)
	local var_17_1 = arg_17_1 * 60
	local var_17_2 = 0
	local var_17_3 = 0

	for iter_17_0 = #var_17_0, 1, -1 do
		var_17_3 = iter_17_0 - 1
		var_17_2 = var_17_2 + var_17_0[iter_17_0]

		if var_17_1 <= var_17_2 then
			break
		end
	end

	return var_17_3
end

function system.get_rollback_durations(arg_18_0)
	local var_18_0 = {}
	local var_18_1 = 0

	while true do
		local var_18_2 = system.get_replay_file_duration(arg_18_0 .. var_18_1 .. ".inputs")

		if not var_18_2 then
			break
		end

		var_18_0[#var_18_0 + 1] = var_18_2
		var_18_1 = var_18_1 + 1
	end

	return var_18_0
end

function system.get_replay_file_duration(arg_19_0)
	local var_19_0 = io.open(arg_19_0, "r")

	if not var_19_0 then
		return
	end

	local var_19_1 = var_19_0:read("*all")

	var_19_0:close()

	return tonumber(var_19_1:match("%(frame (%d+)%)%s*$"))
end

function system.run_gamedemoplay_TEST(arg_20_0)
	system.run_gamedemoplay("CLV-P-VAACJ")
end

function system.run_gamesuperplay(arg_21_0)
	print("run_gamesuperplay")

	local var_21_0 = titles_list[arg_21_0]

	if table_isEmpty(var_21_0.superplay) then
		debugLabelPrint("run_gamedemoplay : superplay is empty or not found")

		return
	end

	local var_21_1 = #var_21_0.superplay

	if not store.info then
		store.info = {}
	end

	if not store.info[arg_21_0] then
		store.info[arg_21_0] = {}
	end

	if not store.info[arg_21_0].super_index then
		store.info[arg_21_0].super_index = 1
	end

	local var_21_2 = store.info[arg_21_0].super_index

	store.info[arg_21_0].super_index = index_loop_next(store.info[arg_21_0].super_index, var_21_1)

	debugLabelPrint("GAME START : MCP Version")

	local var_21_3 = var_21_0.superplay[var_21_2] .. ".state"
	local var_21_4 = var_21_0.superplay[var_21_2]

	if not FileUtils.fileExists(var_21_3) or not FileUtils.fileExists(var_21_4) then
		debugLabelPrint("run_gamesuperplay : Inputs or state file not found")

		return
	end

	local var_21_5 = system.make_display_option_table()

	if HOST_PLATFORM_IS_WINDOWS then
		system.fadeOut()

		return
	end

	local var_21_6 = {}
	local var_21_7 = system.make_frame_option_table()

	system.fadeOut(mcp.start_super_play, arg_21_0, var_21_3, var_21_4, var_21_5, var_21_7)
end

function system.make_display_option_table()
	if not store.setting then
		store.setting = {}
	end

	return {
		mode = store.setting.display
	}
end

function system.make_frame_option_table()
	if not store.setting then
		store.setting = {}
	end

	local var_23_0 = store.setting.display
	local var_23_1 = store.setting.frame_index

	if system.isPlayingDemo() then
		var_23_1 = 1
	end

	if var_23_1 == 1 or not var_23_1 then
		return nil
	end

	local var_23_2 = DECORATIVE_FRAMES_PATH .. "/" .. DecorativeFrames:getFullFrameName(var_23_1 - 1, var_23_0)

	if CLOVER_IS_DEBUG and not FileUtils.fileExists(var_23_2 .. ".png") then
		return nil
	end

	return var_23_2
end

function system.setFadeType(arg_24_0)
	sys_boot.instance.fade:setType(arg_24_0)
end

function system.fadeOut(arg_25_0, ...)
	print("fadeOut")

	system.clock_begin = os.clock()

	sys_boot.instance:fadeOut(arg_25_0, ...)
end

function system.play_count(arg_26_0)
	if store.play_count < var_0_0 then
		store.play_count = store.play_count + 1
	end

	if not store.recently_count then
		store.recently_count = 1
	else
		store.recently_count = store.recently_count + 1
	end

	print("store.recently_count " .. store.recently_count)

	if not store.info then
		store.info = {}
	end

	if not store.info[arg_26_0] then
		store.info[arg_26_0] = {}
	end

	if not store.info[arg_26_0].play_count then
		store.info[arg_26_0].play_count = 1
	else
		store.info[arg_26_0].play_count = store.info[arg_26_0].play_count + 1
	end

	store.info[arg_26_0].recently_number = store.recently_count

	print("store.info[game_code].recently_number " .. store.info[arg_26_0].recently_number)
	print("store.info[game_code].play_count " .. store.info[arg_26_0].play_count)
end

function system.is_hvc()
	if Env.profile == "default" or Env.profile == "jpn" then
		return true
	end

	return false
end

function system.is_nes()
	if Env.profile ~= "default" and Env.profile ~= "jpn" then
		return true
	end

	return false
end

function system.TextureIsLoaded()
	return system.loadingCount == 0 and system.thumbnailIsloading == false
end

function system.setLocale(arg_30_0, arg_30_1, arg_30_2)
	Localization.setLocale(arg_30_0, arg_30_1, arg_30_2)
	Localization.reloadStrings()
	LabelComponent.refreshTextAll()

	for iter_30_0, iter_30_1 in pairs(system.hud) do
		iter_30_1:refresh()
	end

	if HOST_PLATFORM_IS_LINUX then
		local var_30_0 = {
			[PLATFORM_REGION_JAPAN] = "JPN",
			[PLATFORM_REGION_AMERICA] = "USA",
			[PLATFORM_REGION_EUROPE] = "EUR"
		}

		mcp.set_language(var_30_0[arg_30_2], arg_30_0)
	end
end

function system.set_locked_resumedata(arg_31_0, arg_31_1, arg_31_2)
	if not store.info then
		store.info = {}
	end

	if not store.info[arg_31_0] then
		store.info[arg_31_0] = {}
	end

	if not store.info[arg_31_0].locked then
		store.info[arg_31_0].locked = {}
	end

	store.info[arg_31_0].locked[arg_31_1] = arg_31_2
end

function system.is_locked_resumedata(arg_32_0, arg_32_1)
	if store.info and store.info[arg_32_0] and store.info[arg_32_0].locked then
		return store.info[arg_32_0].locked[arg_32_1]
	end

	return false
end

function system.checkClock(arg_33_0)
	if not system.clock_begin then
		system.clock_begin = os.clock()
	end

	if not system.clock then
		system.clock = {}
	end

	system.clock[arg_33_0] = os.clock() - system.clock_begin

	local var_33_0 = ""

	for iter_33_0, iter_33_1 in pairs(system.clock) do
		var_33_0 = var_33_0 .. string.format("%s:%f ", iter_33_0, iter_33_1)
	end

	print(var_33_0)
end

function system.waitBootFade(arg_34_0, arg_34_1)
	system.wait_boot_fade[arg_34_0] = arg_34_1
end

function system.isWaitBootFade()
	for iter_35_0, iter_35_1 in pairs(system.wait_boot_fade) do
		if iter_35_1 then
			return true
		end
	end

	return false
end

function system.setBootRollback(arg_36_0, arg_36_1)
	store.rollback.game_code = arg_36_0
	store.rollback.is_volatile = arg_36_1
end

function system.clearBootRollback()
	store.rollback = {}
end

function system.isStarfoxClear()
	return FileUtils.fileExists(STARFOX_THE_END_FILENAME)
end

function system.isLockStarfox2()
	-- Star Fox 2 only unlocks by clearing Star Fox in the emulator, which is out of scope
	-- for this menu replica, so it is always shown revealed (full box art, no gift-box wrap)
	-- on both the main card and the mini filmstrip icon.
	return false
end

function system.unlockStarfox2()
	store.starfox2_lock = "unlock"
end

function system.lockStarfox2()
	store.starfox2_lock = "lock"
end

function system.isExistsFloatingData()
	if system.floating_save and system.floating_cards then
		return true
	end

	return false
end

function system.isPlayingDemo()
	if not store.autoplay then
		return false
	end

	return store.autoplay.running
end

function system.compareFilesContent(arg_44_0, arg_44_1)
	return FileUtils.readFile(arg_44_0) == FileUtils.readFile(arg_44_1)
end
