require("core/core.lua")

SaveContentHelper = class()
SaveContentHelper.newDirectorySuffix = ".new"
SaveContentHelper.oldDirectorySuffix = ".old"
SaveContentHelper.swappedDirectorySuffix = ".swap"

function SaveContentHelper.EnsureConsistency(arg_1_0, arg_1_1)
	local var_1_0 = arg_1_0.newDirectorySuffix
	local var_1_1 = arg_1_0.oldDirectorySuffix
	local var_1_2 = arg_1_0.swappedDirectorySuffix
	local var_1_3 = string.len(var_1_1)
	local var_1_4 = string.len(var_1_0)
	local var_1_5 = FileUtils.listDirectories(arg_1_1)

	for iter_1_0, iter_1_1 in pairs(var_1_5) do
		local var_1_6 = arg_1_1 .. iter_1_1

		if string.sub(var_1_6, -var_1_4) == var_1_0 then
			local var_1_7 = string.sub(var_1_6, 0, -(var_1_4 + 1))
			local var_1_8 = var_1_7 .. var_1_1

			if FileUtils.directoryExists(var_1_8) then
				print("EnsureConsistency : complete steps 3 & 4")

				if FileUtils.renameDirectory(var_1_6, var_1_7) == false then
					print("Step 3 : Failed to rename '" .. var_1_6 .. "' to '" .. var_1_7 .. "'")

					return
				end

				if FileUtils.deleteDirectory(var_1_8) == false then
					print("Step 4 : Failed to cleanup '" .. var_1_8 .. "'")

					return
				end
			else
				print("EnsureConsistency : restore to previous status")

				if FileUtils.deleteDirectory(var_1_6) == false then
					print("Failed to remove artifacts '" .. var_1_6 .. "'")

					return
				end
			end

			return
		end

		if string.sub(var_1_6, -var_1_3) == var_1_1 then
			local var_1_9 = string.sub(var_1_6, 0, -(var_1_3 + 1))
			local var_1_10 = var_1_9 .. var_1_0

			if not FileUtils.fileExists(var_1_10) then
				print("EnsureConsistency : complete step 4 '" .. var_1_9 .. "'")

				if FileUtils.directoryExists(var_1_9) then
					if FileUtils.deleteDirectory(var_1_6) == false then
						print("Failed to remove artifacts '" .. var_1_9 .. "'")

						return
					end
				else
					FileUtils.renameDirectory(var_1_6, var_1_9)
				end
			end
		end

		if string.sub(var_1_6, -string.len(var_1_2)) == var_1_2 then
			local var_1_11 = 0
			local var_1_12 = string.gmatch(iter_1_1, "[^.]+")
			local var_1_13 = {}

			for iter_1_2 in var_1_12 do
				var_1_11 = var_1_11 + 1
				var_1_13[var_1_11] = iter_1_2
			end

			local var_1_14 = var_1_13[#var_1_13 - 2]
			local var_1_15 = var_1_13[#var_1_13 - 1]
			local var_1_16 = arg_1_1 .. var_1_14
			local var_1_17 = arg_1_1 .. var_1_15

			print("EnsureConsistency : completing unfinished swap (" .. var_1_14 .. " <-> " .. var_1_15 .. ")")

			if not FileUtils.directoryExists(var_1_16) then
				print("EnsureConsistency :  -> restoring '" .. iter_1_1 .. "' as '" .. var_1_14 .. "'")
				FileUtils.renameDirectory(var_1_6, var_1_16)
			else
				print("EnsureConsistency :  -> restoring '" .. iter_1_1 .. "' as '" .. var_1_15 .. "'")
				FileUtils.renameDirectory(var_1_6, var_1_17)
			end
		end
	end
end

function SaveContentHelper.CopyDirectory(arg_2_0, arg_2_1, arg_2_2)
	local var_2_0 = arg_2_2 .. arg_2_0.newDirectorySuffix
	local var_2_1 = arg_2_2 .. arg_2_0.oldDirectorySuffix

	if FileUtils.copyDirectory(arg_2_1, var_2_0) == false then
		print("Step1 : CopyDirectory '" .. arg_2_1 .. "' to '" .. arg_2_2 .. "' failed to copy")

		return false
	end

	if FileUtils.directoryExists(arg_2_2) and FileUtils.renameDirectory(arg_2_2, var_2_1) == false then
		print("Step 2: CopyDirectory '" .. arg_2_1 .. "' to '" .. arg_2_2 .. "' failed to backup previous dir")

		return false
	end

	if FileUtils.renameDirectory(var_2_0, arg_2_2) == false then
		print("Step 3 : CopyDirectory '" .. arg_2_1 .. "' to '" .. arg_2_2 .. "' failed to commit")

		return false
	end

	if FileUtils.directoryExists(var_2_1) and FileUtils.deleteDirectory(var_2_1) == false then
		print("Step 4 : CopyDirectory '" .. arg_2_1 .. "' to '" .. arg_2_2 .. "' failed to cleanup")

		return false
	end

	return true
end

function SaveContentHelper.onAsyncDirectoryCopyCompleted(arg_3_0, arg_3_1, arg_3_2)
	local var_3_0 = arg_3_2.fromDir
	local var_3_1 = arg_3_2.toDir
	local var_3_2 = var_3_1 .. SaveContentHelper.newDirectorySuffix
	local var_3_3 = var_3_1 .. SaveContentHelper.oldDirectorySuffix

	if arg_3_1 and FileUtils.directoryExists(var_3_1) and FileUtils.renameDirectory(var_3_1, var_3_3) == false then
		print("Step 2: CopyDirectoryAsync '" .. var_3_0 .. "' to '" .. var_3_1 .. "' failed to backup previous dir")

		arg_3_1 = false
	end

	if arg_3_1 and FileUtils.renameDirectory(var_3_2, var_3_1) == false then
		print("Step 3 : CopyDirectoryAsync '" .. var_3_0 .. "' to '" .. var_3_1 .. "' failed to commit")

		arg_3_1 = false
	end

	if arg_3_1 and FileUtils.directoryExists(var_3_3) and FileUtils.deleteDirectory(var_3_3) == false then
		print("Step 4 : CopyDirectoryAsync '" .. var_3_0 .. "' to '" .. var_3_1 .. "' failed to cleanup")

		arg_3_1 = false
	end

	if arg_3_2.callback then
		arg_3_2.callback(arg_3_1)
	end
end

function SaveContentHelper.CopyDirectoryAsync(arg_4_0, arg_4_1, arg_4_2, arg_4_3)
	local var_4_0 = arg_4_2 .. arg_4_0.newDirectorySuffix
	local var_4_1 = arg_4_2 .. arg_4_0.oldDirectorySuffix
	local var_4_2 = {
		callback = arg_4_3,
		fromDir = arg_4_1,
		toDir = arg_4_2
	}

	if FileUtils.copyDirectoryAsync(arg_4_1, var_4_0, SaveContentHelper.onAsyncDirectoryCopyCompleted, var_4_2) == false then
		print("Step1 : CopyDirectoryAsync '" .. arg_4_1 .. "' to '" .. arg_4_2 .. "' failed to copy")

		return false
	end

	return true
end

function SaveContentHelper.MoveDirectory(arg_5_0, arg_5_1, arg_5_2)
	if not SaveContentHelper:CopyDirectory(arg_5_1, arg_5_2) then
		print("Failed to CopyDirectory from " .. arg_5_1 .. " to " .. arg_5_2 .. "")

		return false
	end

	if FileUtils.deleteDirectory(arg_5_1) == false then
		print("MoveDirectory '" .. arg_5_1 .. "' to '" .. arg_5_2 .. "' failed to cleanup source")

		return false
	end

	return true
end

function SaveContentHelper.onAsyncDirectoryMoveCompleted(arg_6_0, arg_6_1, arg_6_2)
	local var_6_0 = arg_6_2.fromDir
	local var_6_1 = arg_6_2.toDir
	local var_6_2 = var_6_1 .. SaveContentHelper.newDirectorySuffix
	local var_6_3 = var_6_1 .. SaveContentHelper.oldDirectorySuffix

	if arg_6_1 and FileUtils.directoryExists(var_6_1) and FileUtils.renameDirectory(var_6_1, var_6_3) == false then
		print("Step 2: MoveDirectoryAsync '" .. var_6_0 .. "' to '" .. var_6_1 .. "' failed to backup previous dir")

		arg_6_1 = false
	end

	if arg_6_1 and FileUtils.renameDirectory(var_6_2, var_6_1) == false then
		print("Step 3 : MoveDirectoryAsync '" .. var_6_0 .. "' to '" .. var_6_1 .. "' failed to commit")

		arg_6_1 = false
	end

	if arg_6_1 and FileUtils.directoryExists(var_6_3) and FileUtils.deleteDirectory(var_6_3) == false then
		print("Step 4 : MoveDirectoryAsync '" .. var_6_0 .. "' to '" .. var_6_1 .. "' failed to cleanup")

		arg_6_1 = false
	end

	if arg_6_1 and FileUtils.deleteDirectory(var_6_0) == false then
		print("MoveDirectoryAsync '" .. var_6_0 .. "' to '" .. var_6_1 .. "' failed to cleanup source")

		arg_6_1 = false
	end

	if arg_6_2.callback then
		arg_6_2.callback(arg_6_1)
	end
end

function SaveContentHelper.MoveDirectoryAsync(arg_7_0, arg_7_1, arg_7_2, arg_7_3)
	local var_7_0 = arg_7_2 .. arg_7_0.newDirectorySuffix
	local var_7_1 = arg_7_2 .. arg_7_0.oldDirectorySuffix
	local var_7_2 = {
		callback = arg_7_3,
		fromDir = arg_7_1,
		toDir = arg_7_2
	}

	if FileUtils.copyDirectoryAsync(arg_7_1, var_7_0, SaveContentHelper.onAsyncDirectoryMoveCompleted, var_7_2) == false then
		print("Step1 : MoveDirectoryAsync '" .. arg_7_1 .. "' to '" .. arg_7_2 .. "' failed to initiate async copy")

		return false
	end

	return true
end

function SaveContentHelper.RenameDirectory(arg_8_0, arg_8_1, arg_8_2)
	if FileUtils.renameDirectory(arg_8_1, arg_8_2) == false then
		print("RenameDirectory '" .. arg_8_1 .. "' to '" .. arg_8_2 .. "' failed to rename")

		return false
	end

	return true
end

function SaveContentHelper.SwapDirectories(arg_9_0, arg_9_1, arg_9_2, arg_9_3)
	local var_9_0 = arg_9_1 .. "/"
	local var_9_1 = arg_9_2 .. "." .. arg_9_3 .. arg_9_0.swappedDirectorySuffix
	local var_9_2 = var_9_0 .. arg_9_2
	local var_9_3 = var_9_0 .. arg_9_3
	local var_9_4 = var_9_0 .. var_9_1

	if FileUtils.renameDirectory(var_9_2, var_9_4) == false then
		print("SwapDirectories '" .. arg_9_2 .. "' <-> '" .. arg_9_3 .. "' failed to backup")

		return false
	end

	if FileUtils.renameDirectory(var_9_3, var_9_2) == false then
		print("SwapDirectories '" .. arg_9_2 .. "' <-> '" .. arg_9_3 .. "' failed to rename")

		return false
	end

	if FileUtils.renameDirectory(var_9_4, var_9_3) == false then
		print("SwapDirectories '" .. arg_9_2 .. "' <-> '" .. arg_9_3 .. "' failed to restore")

		return false
	end

	return true
end
