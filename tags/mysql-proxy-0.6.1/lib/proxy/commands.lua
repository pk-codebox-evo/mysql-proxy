--[[

   Copyright (C) 2007 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

--]]

module("proxy.commands", package.seeall)

---
-- map the constants to strings 
-- lua starts at 1
local command_names = {
	"COM_SLEEP",
	"COM_QUIT",
	"COM_INIT_DB",
	"COM_QUERY",
	"COM_FIELD_LIST",
	"COM_CREATE_DB",
	"COM_DROP_DB",
	"COM_REFRESH",
	"COM_SHUTDOWN",
	"COM_STATISTICS",
	"COM_PROCESS_INFO",
	"COM_CONNECT",
	"COM_PROCESS_KILL",
	"COM_DEBUG",
	"COM_PING",
	"COM_TIME",
	"COM_DELAYED_INSERT",
	"COM_CHANGE_USER",
	"COM_BINLOG_DUMP",
	"COM_TABLE_DUMP",
	"COM_CONNECT_OUT",
	"COM_REGISTER_SLAVE",
	"COM_STMT_PREPARE",
	"COM_STMT_EXECUTE",
	"COM_STMT_SEND_LONG_DATA",
	"COM_STMT_CLOSE",
	"COM_STMT_RESET",
	"COM_SET_OPTION",
	"COM_STMT_FETCH",
	"COM_DAEMON"
}

---
-- split a MySQL command packet into its parts
--
-- @param packet a network packet
-- @return a table with .type, .type_name and command specific fields 
function parse(packet)
	local cmd = {}

	cmd.type = packet:byte()
	cmd.type_name = command_names[cmd.type + 1]
	
	if cmd.type == proxy.COM_QUERY then
		cmd.query = packet:sub(2)
	elseif cmd.type == proxy.COM_INIT_DB then
		cmd.schema = packet:sub(2)
	elseif cmd.type == proxy.COM_QUIT then
	elseif cmd.type == proxy.COM_PING then
	elseif cmd.type == proxy.COM_SHUTDOWN then
		-- nothing to decode
	elseif cmd.type == proxy.COM_STMT_PREPARE then
		cmd.query = packet:sub(2)
	elseif cmd.type == proxy.COM_FIELD_LIST then
		-- should have a table-name
	else
		print("[debug] (command) unhandled type " .. cmd.type_name)
	end

	return cmd
end

function pretty_print(cmd)
	if cmd.type == proxy.COM_QUERY or
	   cmd.type == proxy.COM_STMT_PREPARE then
		return ("[%s] %s"):format(cmd.type_name, cmd.query)
	elseif cmd.type == proxy.COM_INIT_DB then
		return ("[%s] %s"):format(cmd.type_name, cmd.schema)
	elseif cmd.type == proxy.COM_QUIT or
	       cmd.type == proxy.COM_PING or
	       cmd.type == proxy.COM_SHUTDOWN then
		return ("[%s]"):format(cmd.type_name)
	elseif cmd.type == proxy.COM_FIELD_LIST then
		-- should have a table-name
		return ("[%s]"):format(cmd.type_name)
	end

	return ("[%s] ... no idea"):format(cmd.type_name)
end
