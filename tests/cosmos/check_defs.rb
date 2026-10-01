# Cross-checks the generated COSMOS definitions against the Python codec.
# Run through tests/cosmos/check_defs.sh, which supplies the Python-encoded packets.
require 'cosmos'
require 'cosmos/packets/packet_config'

dir, packets_file = ARGV
failures = []

pc = Cosmos::PacketConfig.new
pc.process_file(File.join(dir, 'FLATSAT_TLM.txt'), 'FLATSAT')
pc.process_file(File.join(dir, 'FLATSAT_CMD.txt'), 'FLATSAT')
puts "parsed #{pc.telemetry['FLATSAT'].size} telemetry packets and #{pc.commands['FLATSAT'].size} commands"
pc.warnings.each { |w| failures << "COSMOS warning: #{w}" }

# Each line: kind name hex json-of-expected-values
File.readlines(packets_file).each do |line|
  kind, name, hex, expected = line.strip.split(' ', 4)
  expected = JSON.parse(expected)
  bytes = [hex].pack('H*')
  if kind == 'TLM'
    p = pc.telemetry['FLATSAT'][name].clone
    p.buffer = bytes
    failures << "#{name}: not identified by its MID" unless p.identify?(bytes)
    failures << "#{name}: length #{bytes.length} != defined #{p.defined_length}" if bytes.length != p.defined_length
    expected.each do |item, value|
      got = p.read(item)
      ok = value.is_a?(Float) ? (got - value).abs < 1e-6 : got == value
      failures << "#{name}.#{item}: COSMOS read #{got.inspect}, Python wrote #{value.inspect}" unless ok
    end
  else
    c = pc.commands['FLATSAT'][name].clone
    c.restore_defaults
    expected.each { |item, value| c.write(item, value) }
    ours = c.buffer.unpack1('H*')
    # Byte 7 is the checksum, which COSMOS doesn't compute yet (ICD OI-03)
    unless ours[0, 14] == hex[0, 14] && ours[16..-1] == hex[16..-1]
      failures << "#{name}: COSMOS encoded #{ours}, Python encoded #{hex}"
    end
  end
end

if failures.empty?
  puts 'COSMOS definitions match the Python codec'
else
  failures.each { |f| puts "FAIL #{f}" }
  exit 1
end
