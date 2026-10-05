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

step('telemetry arriving (OBC_HK)') { wait_check('FLATSAT OBC_HK UPTIME > 0', 90) }

# NOS3's 2.8 deg/s deployment tip-off starts automatic detumbling; this test is about commanding, so it turns
# automatic mode transitions off (tests/system/test_adcs.py covers them)
step('automatic mode transitions off') do
  cmd("FLATSAT OBC_SET_AUTO_MODES with STATE 'OFF'")
  wait_check('FLATSAT ADCS_STATE AUTO_MODES == 0', 5)
end

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

# The radio path: COSMOS target FLATSAT_RF through the ground station software (FlatSat ICD 7)
step('ground station status on FLATSAT_RF') { wait_check('FLATSAT_RF GS_STATUS RANGE > 0', 15) }

step('contact from COSMOS: beacon over the radio') do
  cmd("FLATSAT_RF GS_SET_CONTACT_MODE with MODE 'ALWAYS', ON_TIME 0, OFF_TIME 0")
  wait_check('FLATSAT_RF GS_STATUS CONTACT == 1', 5)
  wait_check('FLATSAT_RF BEACON UPTIME > 0', 15)
end

step('command over the radio, confirmed in the next beacon') do
  count = tlm('FLATSAT OBC_HK CMD_ACCEPT_COUNT')
  cmd('FLATSAT_RF COMMS_NOOP')
  wait_check("FLATSAT OBC_HK CMD_ACCEPT_COUNT == #{count + 1}", 10)
  wait_check("FLATSAT_RF BEACON CMD_ACCEPT_COUNT == #{count + 1}", 20)
end

# Show what an operator would see
rates = (0..2).map { |i| tlm("FLATSAT ADCS_SENSORS IMU_RATE_#{i}").round(3) } # shown in deg/s
field = (0..2).map { |i| (tlm("FLATSAT ADCS_SENSORS MAG_#{i}") * 1e6).round(2) }
puts "     body rate #{rates} deg/s, magnetic field #{field} uT, " \
     "simulated battery #{tlm('FLATSAT EPS_SIM BATT_V').round(2)} V, uptime #{tlm('FLATSAT OBC_HK UPTIME')} s"
puts "     ground station: elevation #{tlm('FLATSAT_RF GS_STATUS ELEVATION').round(1)} deg, " \
     "next pass in #{(tlm('FLATSAT_RF GS_STATUS NEXT_AOS') / 60.0).round(1)} min, " \
     "#{tlm('FLATSAT_RF GS_STATUS DOWN_FRAMES')} frames received"

passed = $results.count(true)
puts "---- #{passed} passed, #{$results.size - passed} failed ----"
exit(passed == $results.size ? 0 : 1)
