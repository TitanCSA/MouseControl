param([ValidateSet('All','Build','Test','Package')][string]$Target='All',[switch]$AddressSanitizer,[switch]$Render)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$build=Join-Path $root $(if($AddressSanitizer){'obj\Asan'}else{'obj\Release'})
$cache=Join-Path $build 'CMakeCache.txt'
if(Test-Path -LiteralPath $cache){
 $cacheEntry=Select-String -LiteralPath $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$'|Select-Object -First 1
 if($cacheEntry -and ![string]::Equals($cacheEntry.Matches[0].Groups[1].Value.Replace('/','\'),$root,[StringComparison]::OrdinalIgnoreCase)){
  $hash=[Security.Cryptography.SHA256]::Create();try{$id=([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($root)))).Replace('-','').Substring(0,10)}finally{$hash.Dispose()}
  $build+='-'+$id
 }
}
$savedPath=$env:PATH
try{
 if($AddressSanitizer){
  $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  $vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  $runtime=Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory|Where-Object {Test-Path (Join-Path $_.FullName 'bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll')}|Sort-Object {[version]$_.Name} -Descending|Select-Object -First 1
  if(!$runtime){throw 'Install the Visual Studio C++ AddressSanitizer component.'}
  $env:PATH=(Join-Path $runtime.FullName 'bin\Hostx64\x64')+';'+$env:PATH
 }
 if($Target -ne 'Test'){
  & cmake -S $root -B $build -G 'Visual Studio 17 2022' -A x64 ('-DMOUSECONTROL_ASAN='+$(if($AddressSanitizer){'ON'}else{'OFF'}))
  if($LASTEXITCODE){throw 'CMake configuration failed.'}
  & cmake --build $build --config Release --parallel 3
  if($LASTEXITCODE){throw 'C++ build failed.'}
 }
 if($Target -ne 'Build'){
  & ctest --test-dir $build -C Release --output-on-failure --no-tests=error
  if($LASTEXITCODE){throw 'Tests failed. No driver installation or host input capture was performed.'}
 }
 $exe=Join-Path $build 'bin\MouseControl.exe'
 if($Render){
  $image=Join-Path $root 'docs\mousecontrol.png'
  $renderProcess=Start-Process -FilePath $exe -ArgumentList @('--render',('"'+$image+'"')) -PassThru -WindowStyle Hidden
  if(!$renderProcess.WaitForExit(20000) -or $renderProcess.ExitCode){throw 'UI rendering failed.'}
 }
 if($Target -in 'All','Package'){
  if($AddressSanitizer){throw 'Sanitizer builds cannot be distributed.'}
  $installer=Join-Path $root 'InterceptorSetup.exe'
  $expected='54BDD2CB3E5319EEA2588FEC7579721B0CE7B38AB1ACFC888C90B48798F2AC83'
  if(!(Test-Path -LiteralPath $installer)){
   $temporary=Join-Path $root ('obj\setup-download-'+[guid]::NewGuid().ToString('N')+'.exe')
   Invoke-WebRequest -Uri 'https://github.com/TitanCSA/Interceptor-Keyboard-Mouse/releases/download/v1.0.5.0-demo/InterceptorSetup.exe' -OutFile $temporary -UseBasicParsing
   if((Get-FileHash -LiteralPath $temporary).Hash -ne $expected){throw 'Downloaded driver setup failed its pinned SHA256 check.'}
   Move-Item -LiteralPath $temporary -Destination $installer
  }
  if((Get-FileHash -LiteralPath $installer).Hash -ne $expected){throw 'Driver setup does not match the verified Keyboard & Mouse 1.0.5.0 package.'}
  Add-Type -AssemblyName System.IO.Compression
  $files=[ordered]@{'MouseControl.exe'=$exe;'InterceptorSetup.exe'=$installer;'README.txt'=(Join-Path $root 'docs\QUICKSTART.txt');'LICENSE'=(Join-Path $root 'LICENSE');'INTERCEPTOR-LICENSE.txt'=(Join-Path $root 'Interceptor\LICENSE');'DRIVER-LICENSE.txt'=(Join-Path $root 'Interceptor\BINARY-LICENSE.txt')}
  $out=Join-Path $root 'dist';New-Item -ItemType Directory -Force $out|Out-Null
  $zipPath=Join-Path $out 'MouseControl-1.1.0-Windows-x64.zip'
  $stream=[IO.File]::Open($zipPath,[IO.FileMode]::Create)
  try{
   $zip=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create,$true)
   try{foreach($name in $files.Keys){$entry=$zip.CreateEntry($name,[IO.Compression.CompressionLevel]::Optimal);$sourceStream=[IO.File]::OpenRead($files[$name]);$outputStream=$entry.Open();try{$sourceStream.CopyTo($outputStream)}finally{$outputStream.Dispose();$sourceStream.Dispose()}}}finally{$zip.Dispose()}
  }finally{$stream.Dispose()}
  $sum=(Get-FileHash -LiteralPath $zipPath).Hash.ToLowerInvariant()+'  '+[IO.Path]::GetFileName($zipPath)+"`n"+(Get-FileHash -LiteralPath $installer).Hash.ToLowerInvariant()+'  InterceptorSetup.exe'+"`n"
  [IO.File]::WriteAllText((Join-Path $out 'SHA256SUMS.txt'),$sum,[Text.Encoding]::ASCII)
  Write-Host ('Portable package: '+$zipPath)
 }
}finally{$env:PATH=$savedPath}
