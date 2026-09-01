require("/scripts/core/core.lua")
require("/scripts/helpers/savehelper.lua")
require("/scripts/helper_table.lua")
require("/scripts/app/clover_const.lua")
require("/scripts/system.lua")

Store = class()
store = {}
debug_store = {}

local var_0_0 = "system-save.json"
local var_0_1 = "debug-save.json"
local var_0_2 = 7
local var_0_3 = 1
local var_0_4 = 16384

local function var_0_5(arg_1_0, arg_1_1)
	if not Save.exist(arg_1_0) then
		arg_1_1(true, nil)

		return
	end

	local var_1_0 = Save.create(arg_1_0, var_0_4)
	local var_1_1 = system.save_callback_object

	Store.onSaveActionDone_Table[var_1_0] = function(arg_2_0, arg_2_1, arg_2_2, arg_2_3)
		print("Load")

		local var_2_0 = arg_2_1
		local var_2_1 = arg_2_2

		if var_2_0 == 0 then
			local var_2_2 = jsonDecode(var_2_1)

			if var_2_2 then
				table_toNumberKey(var_2_2)
				arg_1_1(true, var_2_2)
			else
				arg_1_1(false, nil)
			end
		else
			arg_1_1(false, nil)
		end

		Save.release(arg_2_0)
	end

	Save.read(var_1_1._ptr, var_1_0)
end

local function var_0_6(arg_3_0, arg_3_1, arg_3_2)
	table_toStringKey(arg_3_1)

	local var_3_0 = jsonEncode(arg_3_1)

	table_toNumberKey(arg_3_1)

	local var_3_1 = Save.create(arg_3_0, var_0_4)
	local var_3_2 = system.save_callback_object

	Store.onSaveActionDone_Table[var_3_1] = function(arg_4_0, arg_4_1, arg_4_2, arg_4_3)
		print("Write")

		if arg_4_1 == 0 then
			arg_3_2(true)
		else
			arg_3_2(false)
		end

		Save.release(arg_4_0)
	end

	Save.write(var_3_2._ptr, var_3_1, var_3_0)
end

local function var_0_7(arg_5_0, arg_5_1)
	local var_5_0 = Save.create(arg_5_0, var_0_4)
	local var_5_1 = system.save_callback_object

	Store.onSaveActionDone_Table[var_5_0] = function(arg_6_0, arg_6_1, arg_6_2, arg_6_3)
		print("Delete")

		if arg_6_1 == 0 then
			arg_5_1(true)
		else
			arg_5_1(false)
		end

		Save.release(arg_6_0)
	end

	Save.remove(var_5_1._ptr, var_5_0)
end

local function var_0_8()
	if not store.info then
		store.info = {}
	end

	if not store.setting then
		store.setting = {}
	end

	if store.setting.myplayDemo == nil then
		store.setting.myplayDemo = true
	end

	if store.setting.autoplayDemo == nil then
		store.setting.autoplayDemo = true
	end

	if store.setting.burn_inPreviention == nil then
		store.setting.burn_inPreviention = true
	end

	if store.setting.frame_index == nil then
		store.setting.frame_index = 1
	end

	if store.setting.frame_scroll == nil then
		store.setting.frame_scroll = 0
	end

	if store.setting.display == nil then
		store.setting.display = "keep-aspect-ratio"
	end

	if not store.autoplay then
		store.autoplay = {}
		store.autoplay.running = false
		store.autoplay.displayOptionCount = CloverConst.Demo.DISPLAY_OPTION_COUNT[store.setting.display]
		store.autoplay.displayOption = store.setting.display
		store.autoplay.to_next_index = false
	end

	if not store.autoplay.myplayIndex then
		store.autoplay.myplayIndex = 1
	end

	if not store.autoplay.demo_phase then
		store.autoplay.demo_phase = 0
	end

	if not store.rollback then
		store.rollback = {}
	end

	if not store.play_count then
		store.play_count = 0
	end

	if not store.myplay_order then
		store.myplay_order = {}
	end

	if not store.sortrule then
		store.sortrule = 1
	end

	if store.language_selected == nil then
		store.language_selected = true
	end

	if not store.starfox2_lock then
		if system.isStarfoxClear() then
			store.starfox2_lock = "unlock"
		else
			store.starfox2_lock = "lock"
		end
	end
end

local function var_0_9()
	uisaveIsBusy = true

	local function var_8_0(arg_9_0, arg_9_1)
		if arg_9_0 then
			if arg_9_1 then
				store = arg_9_1
			end

			var_0_8()
		else
			system.uisave_loadfailed = true

			if REED_DEBUG then
				local var_9_0 = "/var/lib/clover/profiles/0/home-menu/save/" .. var_0_0 .. "/data"

				print("-- menu savefile load failed --")

				if FileUtils.fileExists(var_9_0) then
					print(FileUtils.readFile(var_9_0))
				else
					print("no file")
				end
			end

			Store:delete_uisave()
		end

		uisaveIsBusy = false

		Store:checkSaveRequest()
	end

	var_0_5(var_0_0, var_8_0)
end

local function var_0_10()
	if REED_DEBUG then
		debugsaveIsBusy = true

		local function var_10_0(arg_11_0, arg_11_1)
			if arg_11_0 and arg_11_1 then
				debug_store = arg_11_1
			end

			debugsaveIsBusy = false

			Store:checkSaveRequest()
		end

		var_0_5(var_0_1, var_10_0)
	end
end

local function var_0_11()
	uisaveIsBusy = true

	var_0_6(var_0_0, store, function(arg_13_0)
		if not arg_13_0 then
			system.uisave_savefailed = true
		end

		system.checkClock("uisave")

		uisaveIsBusy = false

		Store:checkSaveRequest()
	end)
end

local function var_0_12()
	if REED_DEBUG then
		debugsaveIsBusy = true

		var_0_6(var_0_1, debug_store, function()
			system.checkClock("debugsave")

			debugsaveIsBusy = false

			Store:checkSaveRequest()
		end)
	end
end

local function var_0_13()
	uisaveIsBusy = true

	var_0_7(var_0_0, function()
		uisaveIsBusy = false

		Store:checkSaveRequest()
	end)
end

local function var_0_14()
	if REED_DEBUG then
		debugsaveIsBusy = true

		var_0_7(var_0_1, function()
			debugsaveIsBusy = false

			Store:checkSaveRequest()
		end)
	end
end

function Store.init(arg_20_0)
	Store:ui_init()
	Store:debug_init()
end

function Store.ui_init(arg_21_0)
	store = {
		setting = {},
		info = {}
	}
end

function Store.debug_init(arg_22_0)
	debug_store = {
		version = var_0_2,
		debug_version = var_0_3
	}
end

function Store.load(arg_23_0)
	if not Store:isBusy() then
		var_0_9()
		var_0_10()
	else
		print("Store is busy")
	end
end

function Store.save(arg_24_0)
	if system.noSave then
		return
	end

	if not Store:isBusy() then
		if not debug_store.noSave then
			var_0_11()
		end

		var_0_12()

		Store.isSaveRequested = false
	else
		print("Store is busy, requesting a save")

		Store.isSaveRequested = true
	end
end

function Store.delete(arg_25_0)
	if not Store:isBusy() then
		Store:init()
		var_0_13()
		var_0_14()
		var_0_8()
	else
		print("Store is busy")
	end
end

function Store.delete_uisave(arg_26_0)
	if not Store:isBusy() then
		Store:ui_init()
		var_0_13()
		var_0_8()
	else
		Store:ui_init()
		var_0_8()
		print("Store is busy")
	end
end

function Store.delete_debugsave(arg_27_0, arg_27_1)
	if not Store:isBusy() then
		Store:debug_init()
		var_0_14()
	else
		print("Store is busy")
	end
end

function Store.isBusy(arg_28_0)
	return uisaveIsBusy or debugsaveIsBusy
end

function Store.checkSaveRequest(arg_29_0)
	if Store.isSaveRequested and not Store:isBusy() then
		Store:save()
	end
end

function Store.checkOldData(arg_30_0)
	if REED_DEBUG then
		if store.version ~= nil then
			print("store.version ~= nil")

			system.uisave_loadfailed = true

			return
		end

		if debug_store.version ~= var_0_2 then
			print("debug_store.version ~= SAVE_VERSION")

			system.uisave_loadfailed = true

			return
		end

		if debug_store.debug_version then
			debug_store.debug_version = 1
		end

		if debug_store.debug_version ~= var_0_3 then
			print("debug_store.debug_version ~= DEBUGSAVE_VERSION")

			system.uisave_loadfailed = true

			return
		end
	end
end

function Store.start(arg_31_0)
	Store.instance = arg_31_0
	Store.onSaveActionDone_Table = {}
	system.save_callback_object = Store.instance.callback_object

	function system.save_callback_object.onSaveActionDone(arg_32_0, arg_32_1, arg_32_2, arg_32_3, arg_32_4)
		Store.onSaveActionDone_Table[arg_32_1](arg_32_1, arg_32_2, arg_32_3, arg_32_4)

		Store.onSaveActionDone_Table[arg_32_1] = nil
	end
end

function Store.stop(arg_33_0)
	if HOST_PLATFORM_IS_WINDOWS then
		arg_33_0:save()
	end
end
