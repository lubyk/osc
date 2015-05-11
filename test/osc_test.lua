--[[------------------------------------------------------

  # osc test

--]]------------------------------------------------------
package.path  = './?.lua;'..package.path
package.cpath = './?.so;' ..package.cpath

local lub    = require 'lub'
local lut    = require 'lut'

local osc    = require  'osc'
local should = lut.Test 'osc'

function should.haveType()
  assertEqual('osc', osc.type)
end

function should.pack()
  local data = osc.pack('/foo/bar', 1, 2, 'hello')
  assertType('string', data)
end

function should.unpack()
  local data = osc.pack('/foo/bar', -2.4, 3.5, 'hello')
  local url, v1, v2, s1 = osc.unpack(data)
  assertEqual('/foo/bar', url)
  assertEqual(-2.4, v1, 0.0000001)
  assertEqual(3.5, v2, 0.0000001)
end

local function testValue(value)
  local url, val = osc.unpack(osc.pack('/some/url', value))
  assertEqual('/some/url', url)
  assertValueEqual(value, val, 0.0000001)
end

function should.packBoolean()
  testValue(true)
  testValue(false)
end

function should.packNumber()
  testValue(1.333)
  testValue(-1000)
end

function should.packString()
  testValue 'holly dog'
end

function should.packArray()
  testValue {-2.4, 3.5, 'hello'}
end

function should.packNestedArray()
  testValue {1, {4,5}, 6}
end

function should.packHash()
  testValue {speed = -2.4, accel = 3.5, name = 'hello'}
end

function should.packNestedHash()
  testValue {
    speed = -2.4, accel = 3.5, name = 'hello',
    sub = {
      lazy = 'dog',
    },
  }
end

should:test()
