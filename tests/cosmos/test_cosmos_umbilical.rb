# End-to-end ground segment test: COSMOS commands the OBC and checks its telemetry over the umbilical.
# Runs inside the COSMOS container next to a headless CmdTlmServer (see test_cosmos_e2e.sh).
require 'cosmos'
require 'cosmos/script'

$results = []

def step(name)
  yield
  $results << true
  puts "PASS #{name}"
rescue StandardError => e
  $results << false
  puts "FAIL #{name}: #{e.message.lines.first.strip}"
end

step('telemetry arriving (OBC_HK in SAFE mode)') { wait_check("FLATSAT OBC_HK MODE == 'SAFE'", 90) }

step('all six sensors valid') { wait_check('FLATSAT ADCS_SENSORS VALID_MASK == 63', 30) }

step('NOOP accepted (checksum filled in by the interface)') do
  count = tlm('FLATSAT OBC_HK CMD_ACCEPT_COUNT')
  cmd('FLATSAT OBC_NOOP')
  wait_check("FLATSAT OBC_HK CMD_ACCEPT_COUNT == #{count + 1}", 5)
  wait_check("FLATSAT OBC_HK CMD_REJECT_COUNT == 0", 1)
end

step('mode change to TEST and back') do
  cmd("FLATSAT OBC_SET_MODE with MODE 'TEST'")
  wait_check("FLATSAT OBC_HK MODE == 'TEST'", 5)
  cmd("FLATSAT OBC_SET_MODE with MODE 'SAFE'")
  wait_check("FLATSAT OBC_HK MODE == 'SAFE'", 5)
  wait_check("FLATSAT OBC_HK MODE_REASON == 'COMMAND'", 1)
end

step('ping reply') do
  cmd('FLATSAT OBC_PING with TOKEN 12648430')
  wait_check('FLATSAT PING_REPLY TOKEN == 12648430', 5)
end

step('event message received') do
  cmd('FLATSAT ADCS_NOOP')
  wait_check("FLATSAT EVENT TEXT == 'ADCS NOOP received'", 5)
end

step('42 truth reaching COSMOS (SIM_42_TRUTH)') { wait_check('SIM_42_TRUTH SIM_42_TRUTH_DATA YEAR == 2025', 10) }

# Show what an operator would see
rates = (0..2).map { |i| (tlm("FLATSAT ADCS_SENSORS IMU_RATE_#{i}") * 180 / Math::PI).round(3) }
field = (0..2).map { |i| (tlm("FLATSAT ADCS_SENSORS MAG_#{i}") * 1e6).round(2) }
puts "     body rate #{rates} deg/s, magnetic field #{field} uT, " \
     "simulated battery #{tlm('FLATSAT EPS_SIM BATT_V').round(2)} V, uptime #{tlm('FLATSAT OBC_HK UPTIME')} s"

passed = $results.count(true)
puts "---- #{passed} passed, #{$results.size - passed} failed ----"
exit(passed == $results.size ? 0 : 1)
