--
-- Update build files for this project
--
package.path  = './?.lua;'..package.path
package.cpath = './?.so;' ..package.cpath

local lut = require 'lut'
local lib = require 'osc'

lut.Builder(lib):make()
