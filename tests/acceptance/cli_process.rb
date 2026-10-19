require 'json'
require 'open3'
require 'tmpdir'

EXECUTABLE, SCENARIO = ARGV
abort 'Expected executable and scenario.' unless EXECUTABLE && SCENARIO

def check(condition, message)
	raise message unless condition
end

def monotonic
	Process.clock_gettime(Process::CLOCK_MONOTONIC)
end

class CliProcess
	def initialize(arguments)
		@input, @output, @error, @wait = Open3.popen3(EXECUTABLE,
			*arguments)
		[@input, @output, @error].each(&:binmode)
		@mutex = Mutex.new
		@changed = ConditionVariable.new
		@buffers = [+'', +'']
		@readers = [@output, @error].each_with_index.map do |pipe, index|
			Thread.new do
				begin
					loop do
						bytes = pipe.readpartial(4096)
						@mutex.synchronize do
							@buffers[index] << bytes
							@changed.broadcast
						end
					end
				rescue EOFError, IOError
				end
			end
		end
	end

	def write(bytes)
		@input.write(bytes)
		@input.flush
	end

	def await(description, timeout: 5)
		deadline = monotonic + timeout
		@mutex.synchronize do
			until yield(*@buffers)
				remaining = deadline - monotonic
				raise "Timed out waiting for #{description}: " \
					"#{@buffers[1]}" unless remaining.positive?
				@changed.wait(@mutex, remaining)
			end
		end
	end

	def alive?
		@wait.alive?
	end

	def finish
		@input.close unless @input.closed?
		raise 'CLI did not exit after EOF.' unless @wait.join(10)
		@readers.each { |thread| thread.join(2) }
		[@buffers[0], @buffers[1], @wait.value.exitstatus]
	ensure
		close
	end

	def close
		@input.close unless @input.closed?
		if @wait.alive?
			begin
				Process.kill('KILL', @wait.pid)
			rescue Errno::ESRCH
			end
			@wait.join(2)
		end
		[@output, @error].each { |pipe| pipe.close unless pipe.closed? }
		@readers.each { |thread| thread.join(2) }
	end
end

def run(arguments, input = ''.b)
	process = CliProcess.new(arguments)
	begin
		process.write(input) unless input.empty?
	rescue Errno::EPIPE
	end
	process.finish
ensure
	process&.close
end

def success(result)
	output, error, status = result
	check(status == 0, "CLI failed (#{status}): #{error}")
	[output, error]
end

def failure(result, status = nil)
	output, error, actual = result
	check(actual && actual != 0, 'Expected CLI failure.')
	check(actual == status, "Expected exit #{status}, got #{actual}: " \
		"#{error}") if status
	check(!error.empty?, 'Expected a diagnostic on stderr.')
	check(output.empty?, 'Failed PDU must not write stdout.')
	error
end

def schema(number = 1, profile = 'D1')
	definition = {
		'format' => 'vmf-schema/1', 'envelope' => profile,
		'messages' => [{
			'message_number' => "K01.#{number}",
			'indexes' => [{'index_number' => '1.', 'number_of_bits' => 8}]
		}]
	}
	definition['standard_version'] = 0 unless profile == 'B'
	definition
end

# Independent minimal envelope construction in the documented logical order.
# The synthetic VMF selector and one-octet body assert no assigned layout.
def packet(profile, sized: true)
	bits = []
	emit = lambda do |value, width = 1|
		width.times { |i| bits << ((value >> i) & 1) }
	end
	d = %w[D D1].include?(profile)
	b = profile == 'B'
	emit.call({'B' => 1, 'C' => 2, 'D' => 3, 'D1' => 4}.fetch(profile), 4)
	4.times { emit.call(0) } # Compression, originator, recipients, information.
	emit.call(0) unless b # HeaderSize absent.
	5.times { emit.call(0) } if d # Future groups 1..5.
	emit.call(0) # Last handling descriptor.
	emit.call(2, 4) # VMF UMF.
	unless b
		emit.call(1)
		emit.call(0, 4) # Explicit raw message-standard zero.
	end
	emit.call(1) # Identification present.
	emit.call(1, 4) # FAD.
	emit.call(1, 7) # Message number.
	emit.call(0) # Subtype absent.
	emit.call(0) # Filename absent.
	emit.call(sized ? 1 : 0)
	emit.call(1, 20) if sized
	emit.call(0, 2) # Operation.
	emit.call(0) # Retransmit.
	emit.call(d ? 0 : 2, 3) # Precedence.
	emit.call(0, 2) # Classification.
	6.times { emit.call(0) } # Marking, DTGs, requests, response, references.
	5.times { emit.call(0) } if d # Future groups 6..10.
	emit.call(0) unless b # Security absent.
	5.times { emit.call(0) } if d # Future groups 11..15.
	bits << 0 until (bits.size % 8).zero?
	header = bits.each_slice(8).map do |octet|
		octet.each_with_index.sum { |bit, i| bit << i }
	end.pack('C*')
	header + "\x5a".b
end

VECTORS = %w[B C D D1].to_h { |profile| [profile, packet(profile)] }
UNSIZED = packet('D1', sized: false)

begin
Dir.mktmpdir('vmf-cli-') do |directory|
	schema_path = File.join(directory, 'schema with spaces.json')
	File.write(schema_path, JSON.generate(schema))
	archive = File.join(directory, 'capture with spaces.jsonl')
	base = ['inspect', '--profile', 'D1', '--schema', schema_path]
	bytes = VECTORS.fetch('D1')
	replay = ['replay', archive, '--schema', schema_path]

	record = lambda do |input = bytes|
		success(run(base + ['--record', archive], input))
		File.readlines(archive).map { |line| JSON.parse(line) }
	end
	write_records = lambda do |records|
		File.write(archive, records.map { |item| JSON.generate(item) }
			.join("\n") + "\n")
	end

	case SCENARIO
	when 'help'
		[['--help'], ['-h'], ['replay', '--help']].each do |arguments|
			output, error = success(run(arguments))
			check(output.include?('vmf'), 'Help must describe vmf.')
			check(error.empty?, 'Help should not diagnose a failure.')
		end
	when 'inspect'
		output, error = success(run(base, bytes))
		check(output.empty?, 'Inspection stdout must be empty by default.')
		check(error.include?('PDU'), 'Expected the default PDU summary.')
		check(!error.include?('FIELD'), 'Default should omit field events.')
		output, = success(run(base.drop(1) + ['--forward'], bytes * 2))
		check(output == bytes * 2, 'Forward must preserve concatenated PDUs.')
		success(run(base, ''.b))
	when 'verbosity'
		levels = ['', '-v', '-vv', '-vvv', '-vvvv']
		outputs = levels.map do |level|
			arguments = base + (level.empty? ? [] : [level])
			output, error = success(run(arguments, bytes))
			check(output.empty?, 'Verbose inspection must keep stdout empty.')
			check(error.include?('PDU'), 'Every level must include summaries.')
			error
		end
		check(outputs[1].include?('ENVELOPE') &&
			outputs[1].include?('MESSAGE_BEGIN'), '-v must show messages.')
		check(!outputs[1].include?('FIELD'), '-v must omit fields.')
		check(outputs[2].include?('FIELD'), '-vv must show fields.')
		check(!outputs[2].include?('GROUP_BEGIN'), '-vv must omit groups.')
		check(outputs[3].include?('GROUP_BEGIN'), '-vvv must show groups.')
		check(outputs[4].include?('WIRE'), '-vvvv must show wire bytes.')
		check(outputs[4].include?('header'),
			'-vvvv must show the complete envelope JSON.')
		_, error = success(run(base + ['-v', '-v', '-v', '-v'], bytes))
		check(error.include?('WIRE'), 'Repeated -v must accumulate.')
	when 'profiles'
		VECTORS.each do |profile, wire|
			File.write(schema_path, JSON.generate(schema(1, profile)))
			arguments = ['--profile', profile, '--schema', schema_path,
				'--forward']
			output, = success(run(arguments, wire))
			check(output == wire, "Profile #{profile} changed bytes.")
		end
	when 'live-pipe'
		process = CliProcess.new(base + ['-vvvv', '--forward'])
		begin
			process.write(bytes.byteslice(0, 4))
			process.write(bytes.byteslice(4, bytes.bytesize - 5))
			process.await('the header before payload and EOF') do |_, error|
				error.include?('ENVELOPE')
			end
			check(process.alive?, 'CLI must await the missing body.')
			process.write(bytes.byteslice(-1, 1))
			process.await('a complete PDU before EOF') do |output, error|
				output == bytes && error.include?('PDU')
			end
			check(process.alive?, 'CLI must await the next PDU.')
			bytes.each_byte { |byte| process.write(byte.chr) }
			process.await('the second PDU before EOF') do |output, _|
				output == bytes * 2
			end
			success(process.finish)
		ensure
			process.close
		end
	when 'record-replay'
		records = record.call(bytes * 2)
		check(records.first == {'format' => 'vmf-events', 'version' => 1,
			'profile' => 'D1', 'schemas' => records.first['schemas']},
			'Unexpected archive prologue.')
		check(records.count { |item| item['event'] == 'PDU_END' } == 2,
			'Expected two complete recorded PDUs.')
		field = records.find { |item| item['event'] == 'FIELD' }
		check(field && field['value'] == {'kind' => 'unsigned',
			'value' => '90'}, 'Record must contain editable scalar events.')
		check(field['index_number'] == [1] && field['depth'] == 1,
			'Record must retain canonical index identity and nesting.')
		output, = success(run(replay))
		check(output == bytes * 2, 'Replay must be byte identical.')
		output, = success(run(['replay', '-', '--schema', schema_path],
			File.binread(archive)))
		check(output == bytes * 2, 'Replay from stdin must be identical.')
		output, error = success(run(replay + ['-vvvv']))
		check(output == bytes * 2, 'Verbose replay must preserve bytes.')
		check(error.include?('WIRE'), '-vvvv replay must show wire bytes.')
	when 'edit-scalar'
		records = record.call
		records.find { |item| item['event'] == 'FIELD' }['value']['value'] = '91'
		write_records.call(records)
		output, = success(run(replay))
		check(output == bytes.byteslice(0, bytes.bytesize - 1) + "\x5b",
			'Editing a scalar must change the corresponding wire value.')
	when 'strict-sizes'
		records = record.call
		envelope = records.find { |item| item['event'] == 'ENVELOPE' }
		envelope['header_octets'] = '999'
		write_records.call(records)
		failure(run(replay))
		records = record.call
		message = records.find { |item| item['event'] == 'MESSAGE_BEGIN' }
		message['payload_octets'] = '2'
		write_records.call(records)
		failure(run(replay))
		records = record.call(bytes * 2)
		envelopes = records.select { |item| item['event'] == 'ENVELOPE' }
		envelopes[1]['header_octets'] = '999'
		write_records.call(records)
		output, error, status = run(replay)
		check(status && status != 0 && !error.empty?,
			'An inconsistent later PDU must fail with a diagnostic.')
		check(output == bytes,
			'Only complete earlier PDUs may be emitted before failure.')
	when 'invalid-archive'
		records = record.call
		records[0]['version'] = 99
		write_records.call(records)
		failure(run(replay))
		records = record.call
		records[0]['schemas'] = []
		write_records.call(records)
		failure(run(replay))
		records = record.call
		records.reject! { |item| item['event'] == 'MESSAGE_BEGIN' }
		write_records.call(records)
		failure(run(replay))
		records = record.call
		records.pop
		write_records.call(records)
		failure(run(replay))
		File.write(archive, "{broken\n")
		failure(run(replay))
	when 'headers-only'
		failure(run(['--profile', 'D1'], bytes))
		output, error = success(run(['--profile', 'D1', '--headers-only',
			'--forward', '-v'], bytes * 2))
		check(output == bytes * 2, 'Header inspection must retain bodies.')
		check(error.scan('ENVELOPE').size >= 2,
			'Header inspection must continue across sized PDUs.')
		failure(run(['--profile', 'D1', '--headers-only',
			'--record', archive], bytes), 2)
		check(!File.exist?(archive), 'Invalid arguments must not create logs.')
		failure(run(['--profile', 'D1', '--headers-only'],
			bytes.byteslice(0, bytes.bytesize - 1)))
		compressed = ['346c000082f8ff0000000000005a'].pack('H*')
		output, = success(run(['--profile', 'D1', '--headers-only',
			'--forward'], compressed))
		check(output == compressed, 'Header inspection must retain transforms.')
		failure(run(base, compressed))
		oversized = UNSIZED.byteslice(0, UNSIZED.bytesize - 1) +
			("\x00" * 1_048_576)
		failure(run(['--profile', 'D1', '--headers-only'], oversized))
		shortened = ['ef00000015000000'].pack('H*')
		success(run(['--profile', 'D1', '--record', archive], shortened))
		output, = success(run(['replay', archive]))
		check(output == shortened,
			'No-schema recordings must replay shortened envelopes.')
	when 'schemas'
		other = File.join(directory, 'other.json')
		File.write(other, JSON.generate(schema(2)))
		output, = success(run(base + ['--schema', other, '--forward'], bytes))
		check(output == bytes, 'Multiple schema files must combine.')
		record.call
		output, = success(run(replay + ['--schema', other]))
		check(output == bytes, 'Unrelated replay schemas must be allowed.')
		changed = schema
		changed['messages'][0]['indexes'][0]['number_of_bits'] = 7
		File.write(schema_path, JSON.generate(changed))
		failure(run(replay))
		File.write(schema_path, JSON.generate(schema))
		failure(run(base + ['--schema', schema_path], bytes))
		failure(run(['--profile', 'D1', '--schema', other], bytes))
		File.write(other, JSON.generate(schema(2, 'C')))
		failure(run(base + ['--schema', other], bytes))
		failure(run(['--profile', 'C', '--schema', schema_path], bytes))
		File.write(other, '{broken')
		failure(run(base + ['--schema', other], bytes))
		failure(run(base + ['--schema', directory]))
	when 'unsized'
		output, = success(run(base + ['--forward'], UNSIZED))
		check(output == UNSIZED, 'EOF-delimited PDU must be forwarded.')
		unsized_records = record.call(UNSIZED)
		output, = success(run(replay))
		check(output == UNSIZED, 'EOF-delimited PDU must replay identically.')
		combined = schema
		combined['messages'].concat(schema(2)['messages'])
		File.write(schema_path, JSON.generate(combined))
		sized_records = record.call
		envelope = unsized_records.find { |item| item['event'] == 'ENVELOPE' }
		envelope['pdu_octets'] = UNSIZED.bytesize.to_s
		write_records.call(unsized_records + sized_records.drop(1))
		failure(run(replay))
	when 'failures'
		[['--bogus'], ['--profile'], ['--profile', 'X'],
			['inspect'], ['replay'], ['replay', archive, '--forward'],
			['--profile', 'D1', '--timed']].each do |arguments|
			failure(run(arguments), 2)
		end
		failure(run(base, bytes.byteslice(0, bytes.bytesize - 1)))
		failure(run(base, "\xff"))
		failure(run(base + ['--schema', File.join(directory, 'absent')]))
		failure(run(base + ['--record', directory], bytes))
		failure(run(replay))
	when 'timed'
		records = record.call(bytes * 2)
		envelopes = records.select { |item| item['event'] == 'ENVELOPE' }
		envelopes[0]['time_ns'] = '0'
		envelopes[1]['time_ns'] = '200000000'
		write_records.call(records)
		output, = success(run(replay))
		check(output == bytes * 2, 'Immediate replay must preserve bytes.')
		started = monotonic
		process = CliProcess.new(replay + ['--timed'])
		begin
			process.await('the first timed PDU') do |wire, _|
				wire.bytesize >= bytes.bytesize
			end
			process.await('the second timed PDU') do |wire, _|
				wire.bytesize == bytes.bytesize * 2
			end
			check(monotonic - started >= 0.12,
				'Timed replay must preserve the recorded gap.')
			success(process.finish)
		ensure
			process.close
		end
	else
		raise "Unknown CLI scenario: #{SCENARIO}"
	end
end
rescue StandardError => error
	warn "CLI #{SCENARIO}: #{error.message}"
	exit 1
end
