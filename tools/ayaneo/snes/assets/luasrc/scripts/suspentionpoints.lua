require("scripts/titles_list.lua")

SuspensionPoint = class()

function SuspensionPoint.initialize(arg_1_0)
	arg_1_0.game_code = ""
	arg_1_0.index = 0
	arg_1_0.thumbnail = nil
	arg_1_0.playtime = 0
	arg_1_0.volatile = false
	arg_1_0.rollback = false
end

function SuspensionPoint.initializeRollback(arg_2_0)
	arg_2_0:initialize()

	arg_2_0.rollback = true
end

function SuspensionPoint.getFolderName(arg_3_0)
	if arg_3_0.rollback then
		return FileUtils.cleanupPath(VOLATILE_SUSPENSION_POINTS_PATH .. "rollback/")
	end

	local var_3_0 = PERSISTENT_SUSPENSION_POINTS_PATH

	if arg_3_0.index == 0 then
		var_3_0 = VOLATILE_SUSPENSION_POINTS_PATH
	end

	return var_3_0 .. arg_3_0.game_code .. "/suspendpoint" .. tostring(arg_3_0.index) .. "/"
end

function SuspensionPoint.getStateFilename(arg_4_0)
	return arg_4_0:getFolderName() .. "state"
end

function SuspensionPoint.getRollbackDataPath(arg_5_0)
	return arg_5_0:getFolderName() .. "rollback/"
end

function SuspensionPoint.getThumbnailFilename(arg_6_0)
	return arg_6_0:getFolderName() .. "state.png"
end

function SuspensionPoint.getPlaytimeFilename(arg_7_0)
	return arg_7_0:getFolderName() .. "state.time"
end

function SuspensionPoint.checkValidity(arg_8_0)
	if (FileUtils.fileExists(arg_8_0:getStateFilename()) or FileUtils.directoryExists(arg_8_0:getRollbackDataPath())) and FileUtils.fileExists(arg_8_0:getThumbnailFilename()) and FileUtils.fileExists(arg_8_0:getPlaytimeFilename()) then
		arg_8_0.valid = true
	else
		arg_8_0.valid = false
	end

	return arg_8_0.valid
end

function SuspensionPoint.isValid(arg_9_0)
	if arg_9_0.valid == nil then
		arg_9_0:checkValidity()
	end

	return arg_9_0.valid
end

function SuspensionPoint.parsePlayTime(arg_10_0)
	local var_10_0 = FileUtils.readFile(arg_10_0:getPlaytimeFilename())

	arg_10_0.playtime = tonumber(var_10_0) or 0

	return arg_10_0.playtime
end

function SuspensionPoint.linkThumbnailResource(arg_11_0)
	local var_11_0 = getImageFromPath(arg_11_0:getThumbnailFilename())

	var_11_0:link()

	arg_11_0.thumbnail_resource = var_11_0
end

MAX_RESUMEDATA_NUM = 4

local var_0_0
local var_0_1 = "/saves"
local var_0_2 = false

SuspentionPoints = class()

function SuspentionPoints.init(arg_12_0)
	local var_12_0 = {}
	local var_12_1 = {}

	for iter_12_0, iter_12_1 in pairs(titles_list) do
		local var_12_2 = iter_12_1.game_code

		if var_12_0[var_12_2] == nil then
			var_12_0[var_12_2] = {}
		end

		SaveContentHelper:EnsureConsistency(PERSISTENT_SUSPENSION_POINTS_PATH .. var_12_2 .. "/")

		iter_12_1.persistent_saves = {}

		arg_12_0:enumerate(PERSISTENT_SUSPENSION_POINTS_PATH .. var_12_2 .. "/", iter_12_1.persistent_saves)

		iter_12_1.volatile_saves = {}

		arg_12_0:enumerate(VOLATILE_SUSPENSION_POINTS_PATH .. var_12_2 .. "/", iter_12_1.volatile_saves)

		iter_12_1.sram_file = PERSISTENT_SUSPENSION_POINTS_PATH .. var_12_2 .. "/cartridge.sram"

		local var_12_3 = {}

		if iter_12_1.persistent_saves then
			for iter_12_2, iter_12_3 in pairs(iter_12_1.persistent_saves) do
				table.insert(var_12_3, iter_12_3)
			end
		end

		if iter_12_1.volatile_saves then
			for iter_12_4, iter_12_5 in pairs(iter_12_1.volatile_saves) do
				table.insert(var_12_3, iter_12_5)
			end
		end

		for iter_12_6, iter_12_7 in ipairs(var_12_3) do
			if debug_store.resumedata_nextdelete and store.nextDelete and FileUtils.cleanupPath(store.nextDelete) == FileUtils.cleanupPath(iter_12_7.folder_name .. "/rollback/") then
				print(("nextDelete %s"):format(iter_12_7.folder_name))

				local var_12_4, var_12_5 = pcall(system.delete_resumedata, iter_12_7.folder_name)

				if not var_12_4 then
					system.resume_deletefailed = true
				end

				store.nextDelete = nil
			else
				local var_12_6 = new(SuspensionPoint)

				var_12_6:initialize()

				var_12_6.game_code = var_12_2
				var_12_6.index = iter_12_7.index
				var_12_6.folder_name = var_12_6:getFolderName()
				var_12_6.state_name = var_12_6:getStateFilename()
				var_12_6.thumbnail = var_12_6:getThumbnailFilename()

				if var_12_6:isValid() then
					var_12_6:parsePlayTime()
					var_12_6:linkThumbnailResource()
					table.insert(var_12_1, var_12_6.state_name)

					if store.last_suspentionpoint and TITLE_RETURN_CODE == -3 and FileUtils.cleanupPath(store.last_suspentionpoint) == var_12_6:getRollbackDataPath() then
						print(("broken %s"):format(var_12_6:getRollbackDataPath()))

						var_12_6.broken = true
						store.last_suspentionpoint = nil
					end

					if var_12_6.index and var_12_6.index >= 1 and var_12_6.index <= MAX_RESUMEDATA_NUM then
						var_12_0[var_12_2][var_12_6.index] = var_12_6
					elseif var_12_6.index == 0 then
						if PREVIOUS_COMMAND == "" and LAST_SHUTDOWN_REASON == "normal" then
							print(("normal shutdown %s"):format(var_12_6.folder_name))

							local var_12_7, var_12_8 = pcall(system.delete_resumedata, var_12_6.folder_name)

							if not var_12_7 then
								system.resume_deletefailed = true
							end
						else
							var_12_0[var_12_2].floating = var_12_6
							system.floating_save = var_12_6
						end
					else
						local var_12_9, var_12_10 = pcall(system.delete_resumedata, var_12_6.folder_name)

						print(var_12_10)
					end
				else
					print(("deleting invalid suspension point folder %s"):format(var_12_6.folder_name))
					FileUtils.deleteDirectory(var_12_6.folder_name)
				end
			end
		end

		if store.info and store.info[var_12_2] and store.info[var_12_2].locked then
			for iter_12_8 = 1, 4 do
				if not var_12_0[var_12_2][iter_12_8] and store.info[var_12_2].locked[iter_12_8] then
					store.info[var_12_2].locked[iter_12_8] = nil
				end
			end
		end
	end

	if FileUtils.directoryExists(VOLATILE_SUSPENSION_POINTS_PATH .. "rollback") then
		system.is_roll_back_return = false
		system.is_roll_back_cancel = false

		local var_12_11 = FileUtils.cleanupPath(VOLATILE_SUSPENSION_POINTS_PATH .. "rollback")

		if PREVIOUS_COMMAND == "resume-game" then
			system.is_roll_back_return = true

			print(var_12_11)

			local var_12_12 = new(SuspensionPoint)

			var_12_12:initializeRollback()

			if not var_12_12:isValid() then
				system.is_roll_back_cancel = true
			end
		end

		if system.is_roll_back_return == true and system.is_roll_back_cancel == false then
			if system.floating_save then
				local var_12_13, var_12_14 = pcall(system.delete_resumedata, system.floating_save.folder_name)

				if not var_12_13 then
					system.resume_deletefailed = true
				end

				titles_list[system.floating_save.game_code].volatile_saves = {}
				var_12_0[system.floating_save.game_code].floating = nil
				system.floating_save = nil
			end

			local var_12_15 = store.rollback.game_code
			local var_12_16 = VOLATILE_SUSPENSION_POINTS_PATH .. var_12_15

			if not FileUtils.directoryExists(var_12_16) then
				FileUtils.createDirectory(var_12_16)
			end

			local var_12_17 = var_12_16 .. "/suspendpoint0"
			local var_12_18, var_12_19 = pcall(system.move_savecontents, var_12_11, var_12_17)

			if not var_12_18 then
				system.resume_deletefailed = true
			end

			local var_12_20 = titles_list[var_12_15]

			var_12_20.volatile_saves = {}

			arg_12_0:enumerate(VOLATILE_SUSPENSION_POINTS_PATH .. var_12_15 .. "/", var_12_20.volatile_saves)

			local var_12_21 = {}

			for iter_12_9, iter_12_10 in pairs(var_12_20.volatile_saves) do
				var_12_21 = iter_12_10
			end

			local var_12_22 = new(SuspensionPoint)

			var_12_22:initialize()

			var_12_22.game_code = var_12_15
			var_12_22.index = var_12_21.index
			var_12_22.folder_name = var_12_22:getFolderName()
			var_12_22.state_name = var_12_22:getStateFilename()
			var_12_22.thumbnail = var_12_22:getThumbnailFilename()

			if var_12_22:isValid() then
				var_12_22:parsePlayTime()
				var_12_22:linkThumbnailResource()

				var_12_0[var_12_15].floating = var_12_22
				system.floating_save = var_12_22
			end
		end

		if FileUtils.directoryExists(VOLATILE_SUSPENSION_POINTS_PATH .. "rollback") then
			local var_12_23, var_12_24 = pcall(system.delete_resumedata, VOLATILE_SUSPENSION_POINTS_PATH .. "rollback")

			if not var_12_23 then
				system.resume_deletefailed = true
			end
		end
	end

	if system.floating_save and not titles_list[system.floating_save.game_code] then
		system.floating_save = nil
	end

	var_0_0 = var_12_0
	var_0_2 = true
	store.nextDelete = nil
	store.last_suspentionpoint = nil
end

function SuspentionPoints.enumerate(arg_13_0, arg_13_1, arg_13_2)
	local var_13_0 = FileUtils.listDirectories(arg_13_1)

	for iter_13_0, iter_13_1 in pairs(var_13_0) do
		local var_13_1 = string.find(iter_13_1, "suspendpoint")

		if var_13_1 then
			local var_13_2 = string.sub(iter_13_1, var_13_1 + 12)
			local var_13_3 = tonumber(var_13_2)

			if var_13_3 ~= nil then
				arg_13_2[var_13_3] = {
					index = var_13_3,
					folder_name = iter_13_1
				}
			end
		end
	end
end

function SuspentionPoints.hasSuspentionPoint(arg_14_0, arg_14_1)
	return not table_isEmpty(var_0_0[arg_14_1])
end

function SuspentionPoints.getInfo(arg_15_0, arg_15_1, arg_15_2)
	return var_0_0[arg_15_1][arg_15_2]
end

function SuspentionPoints.forEachNormal(arg_16_0, arg_16_1)
	for iter_16_0, iter_16_1 in pairs(var_0_0) do
		for iter_16_2, iter_16_3 in pairs(iter_16_1) do
			if iter_16_2 ~= "floating" then
				arg_16_1(iter_16_0, iter_16_2)
			end
		end
	end
end

function SuspentionPoints.moveSlot(arg_17_0, arg_17_1, arg_17_2, arg_17_3, arg_17_4)
	if not store.info then
		store.info = {}
	end

	if not store.info[arg_17_1] then
		store.info[arg_17_1] = {}
	end

	if not store.info[arg_17_1].locked then
		store.info[arg_17_1].locked = {}
	end

	local var_17_0 = store.info[arg_17_1].locked
	local var_17_1 = var_0_0[arg_17_1]
	local var_17_2

	if var_17_1[arg_17_2] then
		var_17_2 = var_17_1[arg_17_2].folder_name
	end

	local var_17_3

	if var_17_1[arg_17_3] then
		local var_17_4 = var_17_1[arg_17_3].folder_name
	end

	system.set_locked_resumedata(arg_17_1, arg_17_3, nil)

	var_17_1[arg_17_3] = nil

	if var_17_2 then
		local var_17_5

		if arg_17_2 == "floating" then
			var_17_5 = system.move_resumedata(var_17_2, arg_17_1, arg_17_3, arg_17_4)
		else
			var_17_5 = system.rename_resumedata(var_17_2, arg_17_1, arg_17_3)
		end

		var_17_1[arg_17_3] = var_17_1[arg_17_2]
		var_17_1[arg_17_2] = nil
		var_17_1[arg_17_3].index = arg_17_3

		local var_17_6 = system.is_locked_resumedata(arg_17_1, arg_17_2)

		system.set_locked_resumedata(arg_17_1, arg_17_3, var_17_6)
		system.set_locked_resumedata(arg_17_1, arg_17_2, nil)

		if var_17_5 ~= var_17_2 then
			var_17_1[arg_17_3].folder_name = var_17_1[arg_17_3]:getFolderName()
			var_17_1[arg_17_3].state_name = var_17_1[arg_17_3]:getStateFilename()
			var_17_1[arg_17_3].thumbnail = var_17_1[arg_17_3]:getThumbnailFilename()
		end
	end
end

function SuspentionPoints.lockSlot(arg_18_0, arg_18_1, arg_18_2, arg_18_3)
	if not store.info then
		store.info = {}
	end

	if not store.info[arg_18_1] then
		store.info[arg_18_1] = {}
	end

	if not store.info[arg_18_1].locked then
		store.info[arg_18_1].locked = {}
	end

	local var_18_0 = "floating"
	local var_18_1 = var_0_0[arg_18_1]
	local var_18_2

	if var_18_1[var_18_0] then
		var_18_2 = var_18_1[var_18_0].folder_name
	end

	if var_18_2 then
		system.move_resumedata(var_18_2, arg_18_1, arg_18_2, arg_18_3)
	end
end

function SuspentionPoints.lockSlotSuccess(arg_19_0, arg_19_1, arg_19_2)
	if not store.info then
		store.info = {}
	end

	if not store.info[arg_19_1] then
		store.info[arg_19_1] = {}
	end

	if not store.info[arg_19_1].locked then
		store.info[arg_19_1].locked = {}
	end

	local var_19_0 = store.info[arg_19_1].locked
	local var_19_1 = "floating"
	local var_19_2 = var_0_0[arg_19_1]

	system.set_locked_resumedata(arg_19_1, arg_19_2, nil)

	var_19_2[arg_19_2] = nil
	var_19_2[arg_19_2] = var_19_2[var_19_1]
	var_19_2[var_19_1] = nil
	var_19_2[arg_19_2].index = arg_19_2

	local var_19_3 = system.is_locked_resumedata(arg_19_1, var_19_1)

	system.set_locked_resumedata(arg_19_1, arg_19_2, var_19_3)
	system.set_locked_resumedata(arg_19_1, var_19_1, nil)

	var_19_2[arg_19_2].folder_name = var_19_2[arg_19_2]:getFolderName()
	var_19_2[arg_19_2].state_name = var_19_2[arg_19_2]:getStateFilename()
	var_19_2[arg_19_2].thumbnail = var_19_2[arg_19_2]:getThumbnailFilename()
end

function SuspentionPoints.swapSlots(arg_20_0, arg_20_1, arg_20_2, arg_20_3)
	if arg_20_2 == "floating" or arg_20_3 == "floating" then
		return
	end

	local var_20_0 = var_0_0[arg_20_1]
	local var_20_1

	if var_20_0[arg_20_2] then
		var_20_1 = var_20_0[arg_20_2].folder_name
	end

	local var_20_2

	if var_20_0[arg_20_3] then
		var_20_2 = var_20_0[arg_20_3].folder_name
	end

	if var_20_1 == nil and var_20_2 == nil then
		return
	end

	if var_20_1 == nil then
		return SuspentionPoints:moveSlot(arg_20_1, arg_20_3, arg_20_2)
	end

	if var_20_2 == nil then
		return SuspentionPoints:moveSlot(arg_20_1, arg_20_2, arg_20_3)
	end

	system.swap_resumedata(arg_20_1, arg_20_2, arg_20_3)

	var_20_0[arg_20_2].index = arg_20_3
	var_20_0[arg_20_3].index = arg_20_2
	var_20_0[arg_20_2], var_20_0[arg_20_3] = var_20_0[arg_20_3], var_20_0[arg_20_2]
	var_20_0[arg_20_2].folder_name = var_20_0[arg_20_2]:getFolderName()
	var_20_0[arg_20_2].state_name = var_20_0[arg_20_2]:getStateFilename()
	var_20_0[arg_20_2].thumbnail = var_20_0[arg_20_2]:getThumbnailFilename()
	var_20_0[arg_20_3].folder_name = var_20_0[arg_20_3]:getFolderName()
	var_20_0[arg_20_3].state_name = var_20_0[arg_20_3]:getStateFilename()
	var_20_0[arg_20_3].thumbnail = var_20_0[arg_20_3]:getThumbnailFilename()

	local var_20_3 = system.is_locked_resumedata(arg_20_1, arg_20_2)
	local var_20_4 = system.is_locked_resumedata(arg_20_1, arg_20_3)

	system.set_locked_resumedata(arg_20_1, arg_20_2, var_20_4)
	system.set_locked_resumedata(arg_20_1, arg_20_3, var_20_3)
end

function SuspentionPoints.deleteSlot(arg_21_0, arg_21_1, arg_21_2)
	local var_21_0 = var_0_0[arg_21_1]
	local var_21_1 = var_21_0[arg_21_2].folder_name

	system.delete_resumedata(var_21_1)
	system.set_locked_resumedata(arg_21_1, arg_21_2, nil)

	var_21_0[arg_21_2] = nil
end
