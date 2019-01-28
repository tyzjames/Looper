#include <limits>

#include "Util.h"
#include "SDAudioRecorder.h"

constexpr const char* RECORDING_FILENAME1 = "RECORD1.RAW";
constexpr const char* RECORDING_FILENAME2 = "RECORD2.RAW";


SD_AUDIO_RECORDER::SD_AUDIO_RECORDER() :
  AudioStream( 1, m_input_queue_array ),
  m_just_played_block( nullptr ),
  m_mode( MODE::STOP ),
  m_play_back_filename( RECORDING_FILENAME1 ),
  m_record_filename( RECORDING_FILENAME1 ),
  m_recorded_audio_file(),
  m_play_back_audio_file(),
  m_play_back_file_size(0),
  m_play_back_file_offset(0),
  m_jump_position(0),
  m_jump_pending(false),
  m_looping(false),
  m_sd_record_queue(*this)
{

}

void SD_AUDIO_RECORDER::update()
{  
  //Serial.print( "update() MODE:" );
  //Serial.println( mode_to_string( m_mode ) );
    
  switch( m_mode )
  {
    case MODE::PLAY:
    {
      if( m_jump_pending )
      {
        if( m_play_back_audio_file.seek( m_jump_position ) )
        {
          m_jump_pending = false;
          m_play_back_file_offset = m_jump_position;
        }
      }
      
      const bool finished = update_playing();

      m_sd_record_queue.update();

      if( finished )
      {
        if( m_looping )
        {
          __disable_irq();
    
          Serial.println("Play - loop");
    
          start_playing();
          m_mode = MODE::PLAY;
          
          __enable_irq();
        }
        else
        {
          m_mode = MODE::STOP;
        }
      }
      break; 
    }
    case MODE::RECORD_INITIAL:
    {
      // update after updating play to capture buffer for overdub
      m_sd_record_queue.update();
        
      update_recording();

      break;
    }
    case MODE::RECORD_PLAY:
    case MODE::RECORD_OVERDUB:
    { 
      const bool finished = update_playing();

      // update after updating play to capture buffer for overdub
      m_sd_record_queue.update();
        
      update_recording();

      // has the loop just finished
      if( finished )
      {
        __disable_irq();

        switch_play_record_buffers();

        stop_recording();
        start_playing();
        start_recording();

        __enable_irq();
      }

      break;
    }
    default:
    {
      break;
    }
  }
}

SD_AUDIO_RECORDER::MODE SD_AUDIO_RECORDER::mode() const
{
  return m_mode;
}
  
void SD_AUDIO_RECORDER::play()
{
  Serial.println("SD_AUDIO_RECORDER::play()");

  AudioNoInterrupts();
  
  play_file( m_play_back_filename, true );

  AudioInterrupts();
}

void SD_AUDIO_RECORDER::play_file( const char* filename, bool loop )
{
  // NOTE - should this be delaying call to start_playing() until update?
  m_play_back_filename = filename;
  m_looping = loop;

  if( m_mode != MODE::PLAY )
  {
    Serial.println("Stop play named file");
    stop_current_mode( false );
  }

  __disable_irq();
  
  if( start_playing() )
  {
    m_mode = MODE::PLAY;
  }
  else
  {
    m_mode = MODE::STOP;
  }
  
  __enable_irq();
}

void SD_AUDIO_RECORDER::stop()
{
  AudioNoInterrupts();
  
  Serial.print("SD_AUDIO_RECORDER::stop() ");
  Serial.println( mode_to_string(m_mode) );
  
  stop_current_mode( true );

  m_mode = MODE::STOP;

  AudioInterrupts();
}

void SD_AUDIO_RECORDER::start_record()
{
  AudioNoInterrupts();
    
  switch( m_mode )
  {
    case MODE::STOP:
    {
      m_play_back_filename  = RECORDING_FILENAME1;
      m_record_filename     = RECORDING_FILENAME2;

      __disable_irq();    // is disabling irq necessary?
      start_recording();
      __enable_irq();

      m_mode = MODE::RECORD_INITIAL;
      
      break;
    }
    case MODE::RECORD_PLAY:
    {
      m_mode = MODE::RECORD_OVERDUB;
      
      break;
    }
    default:
    {
      Serial.print( "SD_AUDIO_RECORDER::start_record() - Invalid mode: " );
      Serial.println( mode_to_string( m_mode ) );
      break;
    }   
  }

  AudioInterrupts();
}

void SD_AUDIO_RECORDER::stop_record()
{
  AudioNoInterrupts();
  
  switch( m_mode )
  {
    case MODE::RECORD_INITIAL:
    {
      __disable_irq();    // is disabling irq necessary?
      stop_recording();

      switch_play_record_buffers();

      start_playing();
      start_recording();
      __enable_irq();

      m_mode = MODE::RECORD_PLAY;
        
      break;
    }
    case MODE::RECORD_OVERDUB:
    {
      m_mode = MODE::RECORD_PLAY;
      break;      
    }
    default:
    {
      Serial.print( "SD_AUDIO_RECORDER::start_record() - Invalid mode: " );
      Serial.println( mode_to_string( m_mode ) );
      break;
    }   
  }

  AudioInterrupts();
}

void SD_AUDIO_RECORDER::set_read_position( float t )
{
 if( m_mode == MODE::PLAY )
 {
  const uint32_t block_size   = 2; // AUDIO_BLOCK_SAMPLES
  const uint32_t file_pos     = m_play_back_file_size * t;
  const uint32_t block_rem    = file_pos % block_size;
  
  
  m_jump_pending  = true;
  m_jump_position = file_pos + block_rem;
 }
}

audio_block_t* SD_AUDIO_RECORDER::aquire_block_func()
{
  // if overdubbing, add incoming audio, otherwise re-record the original audio
  if( m_mode == MODE::RECORD_PLAY )
  {
      ASSERT_MSG( m_just_played_block != nullptr, "Cannot record play, no block" ); // can it be null if overdub exceeds original play file?  

      audio_block_t* play_block = m_just_played_block;
      m_just_played_block = nullptr;

      return play_block;
  }
  if( m_mode == MODE::RECORD_OVERDUB )
  {
    ASSERT_MSG( m_just_played_block != nullptr, "Cannot overdub, no just_played_block" ); // can it be null if overdub exceeds original play file?
    audio_block_t* in_block = receiveWritable();
    ASSERT_MSG( in_block != nullptr, "Overdub - unable to receive block" );

    // mix incoming audio with recorded audio ( from update_playing() ) then release
    if( in_block != nullptr && m_just_played_block != nullptr )
    {
      for( int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i )
      {
        // TODO apply soft clipping?
        in_block->data[i] += m_just_played_block->data[i];
        ASSERT_MSG( in_block->data[i] < std::numeric_limits<int16_t>::max() && in_block->data[i] > std::numeric_limits<int16_t>::min(), "CLIPPING" );
      }
    }
    else
    {
      //ASSERT_MSG( in_block != nullptr, "SD_AUDIO_RECORDER::aquire_block_func() no in_block" );
      ASSERT_MSG( m_just_played_block != nullptr, "SD_AUDIO_RECORDER::aquire_block_func() no just_played_block" );

      audio_block_t* play_block = m_just_played_block;
      m_just_played_block = nullptr;

      return play_block;
    }

    if( m_just_played_block != nullptr )
    {
      release( m_just_played_block );
      m_just_played_block = nullptr;
    }

    return in_block;
  }
  else
  {
    audio_block_t* in_block = receiveReadOnly();
    ASSERT_MSG( in_block != nullptr, "Play/Record Initial - unable to receive block" );
    return in_block;
  }
}

void SD_AUDIO_RECORDER::release_block_func(audio_block_t* block)
{
  release(block);
}

bool SD_AUDIO_RECORDER::start_playing()
{
  Serial.print("SD_AUDIO_RECORDER::start_playing ");
  Serial.println( m_play_back_filename );
  
  // copied from https://github.com/PaulStoffregen/Audio/blob/master/play_sd_raw.cpp
  stop_playing();

  enable_SPI_audio();

  m_play_back_audio_file = SD.open( m_play_back_filename );
  
  if( !m_play_back_audio_file )
  {
    Serial.print("Unable to open file: ");
    Serial.println( m_play_back_filename );
#if defined(HAS_KINETIS_SDHC)
      if (!(SIM_SCGC3 & SIM_SCGC3_SDHC)) AudioStopUsingSPI();
#else
      AudioStopUsingSPI();
#endif

    return false;
  }

  Serial.print("Play File loaded ");
  Serial.println(m_play_back_filename);
  m_play_back_file_size = m_play_back_audio_file.size();
  m_play_back_file_offset = 0;
  Serial.print("File open - file size: ");
  Serial.println(m_play_back_file_size);

  return true;
}

bool SD_AUDIO_RECORDER::update_playing()
{
  bool finished = false;
  const bool set_just_played_block = is_recording();

  // allocate the audio blocks to transmit
  audio_block_t* block = allocate();
  if( block == nullptr )
  {
    Serial.println( "Failed to allocate" );
    return false;
  }

  if( m_play_back_audio_file.available() )
  {
    // we can read more data from the file...
    const uint32_t n = m_play_back_audio_file.read( block->data, AUDIO_BLOCK_SAMPLES*2 );
    m_play_back_file_offset += n;
    for( int i = n/2; i < AUDIO_BLOCK_SAMPLES; i++ )
    {
      block->data[i] = 0;
    }
    transmit(block);
  }
  else
  {
    Serial.println("File End");
    m_play_back_audio_file.close();
    
    disable_SPI_audio();
    
    finished = true;
  }

  if( set_just_played_block )
  {
    ASSERT_MSG( m_just_played_block == nullptr, "Leaking just_played_block" );
    m_just_played_block = block;
  }
  else
  {
    release(block);
  }

  return finished;
}

void SD_AUDIO_RECORDER::stop_playing()
{
  Serial.println("SD_AUDIO_RECORDER::stop_playing");
  
  if( m_mode == MODE::PLAY || m_mode == MODE::RECORD_PLAY || m_mode == MODE::RECORD_OVERDUB )
  {    
    m_play_back_audio_file.close();
    
    disable_SPI_audio();
  }
}

void SD_AUDIO_RECORDER::start_recording()
{  
  Serial.print("SD_AUDIO_RECORDER::start_recording ");
  Serial.println(m_record_filename);
  if( SD.exists( m_record_filename ) )
  {
    // delete previously existing file (SD library will append to the end)
    SD.remove( m_record_filename ); 
  } 
  
  m_recorded_audio_file = SD.open( m_record_filename, FILE_WRITE );

  if( m_recorded_audio_file )
  {
    m_sd_record_queue.start();
    Serial.print("Start recording: ");
    Serial.println( m_record_filename );
  }
  else
  {
    Serial.print("Unable to open file: ");
    Serial.println( m_record_filename );
  }
}

void SD_AUDIO_RECORDER::update_recording()
{
  if( m_sd_record_queue.available() >= 2 )
  {
    byte buffer[512]; // arduino library most efficient with full 512 sector size writes

    // write 2 x 256 byte blocks to buffer
    memcpy( buffer, m_sd_record_queue.read_buffer(), 256);
    m_sd_record_queue.release_buffer();
    memcpy( buffer + 256, m_sd_record_queue.read_buffer(), 256);
    m_sd_record_queue.release_buffer();

    m_recorded_audio_file.write( buffer, 512 );
  }
}

void SD_AUDIO_RECORDER::stop_recording()
{
  Serial.println("SD_AUDIO_RECORDER::stop_recording");
  m_sd_record_queue.stop();

  if( is_recording() )
  {
    // empty the record queue
    while( m_sd_record_queue.available() > 0 )
    {
      Serial.println("Writing final blocks");
      m_recorded_audio_file.write( reinterpret_cast<byte*>(m_sd_record_queue.read_buffer()), 256 );
      m_sd_record_queue.release_buffer();
    }

    m_recorded_audio_file.close();
  }
}

void SD_AUDIO_RECORDER::stop_current_mode( bool reset_play_file )
{
  __disable_irq();
  
  switch( m_mode )
  {
    case MODE::PLAY:
    {
      stop_playing();
      break; 
    }
    case MODE::RECORD_INITIAL:
    {
      stop_recording();
      break;
    }
    case MODE::RECORD_PLAY:
    case MODE::RECORD_OVERDUB:
    {
      stop_playing();
      stop_recording();
      break;
    }
    default:
    {
      break;
    }
  }

  if( reset_play_file )
  {
    m_play_back_filename = m_record_filename = RECORDING_FILENAME1;
  }

  __enable_irq();
}

void SD_AUDIO_RECORDER::switch_play_record_buffers()
{
  // toggle record/play filenames
  swap( m_play_back_filename, m_record_filename );

  Serial.print( "switch_play_record_buffers() Play: ");
  Serial.print( m_play_back_filename );
  Serial.print(" Record: " );
  Serial.println( m_record_filename );
}

const char* SD_AUDIO_RECORDER::mode_to_string( MODE mode )
{
  switch( mode )
  {
    case MODE::PLAY:
    {
      return "PLAY";
    }
    case MODE::STOP:
    {
      return "STOP";
    }
    case MODE::RECORD_INITIAL:
    {
      return "RECORD_INITIAL";
    }
    case MODE::RECORD_PLAY:
    {
      return "RECORD_PLAY";
    }
    case MODE::RECORD_OVERDUB:
    {
      return "RECORD_OVERDUB";
    }
    default:
    {
      return nullptr;
    }
  }
}

uint32_t SD_AUDIO_RECORDER::play_back_file_time_ms() const
{
  const uint64_t num_samples = m_play_back_file_size / 2;
  const uint64_t time_in_ms = ( num_samples * 1000 ) / AUDIO_SAMPLE_RATE;

  Serial.print("Play back time in seconds:");
  Serial.println(time_in_ms / 1000.0f);

  return time_in_ms;
}

