/* API SERVICE LAYER - the only module the UI uses to reach the backend.
   REST CONTRACT v1.1 - implemented by backend/server.cpp on top of the C++ structs:
     GET  /config                         -> {maxScore,maxOtpAttempts,pendingTtlSeconds,queueCapacity,maxAmount,amountDecimals}
     POST /auth/login {username,password} -> {token,user}              [users.csv, REST layer]
     GET  /transactions                   -> Transaction[]
     GET  /cards  POST /cards             (no card lock in v1)
     GET  /locations                      -> string[]
     GET  /locations/route?from=&to=      -> RouteResult {found,totalKm,path[]}
     GET  /cards/{id}/trail               -> CardTrail {steps[{location,timestamp,kmFromPrevious}],totalKm,distinctLocations}
     POST /transactions/start {id?,cardNo,amount,merchant,location,deviceId} -> PendingResult
          optional header Idempotency-Key; id optional (server allocates); amount: number, max 2 decimals
     POST /transactions/{pendingId}/otp {code} | /commit | /cancel   -> PendingResult
     GET  /review-queue -> {capacity,size,items[QueueItem]}   POST /review-queue/front/pop | /rear/pop -> {removed|null}
   PendingResult = {status, pendingId, otpRequired, failedAttempts, attemptsRemaining, expiresInSeconds, txn, prelim, outcome}
     status is the backend PendingStatus name (Started, WrongOtp, Committed, ...).
     expiresInSeconds (backend field, D1) is the whole seconds left while the pending entry exists; null otherwise.
     otpRequired and failedAttempts are null when the backend does not set them (refused cancel).
   Errors: non-2xx with {"error":{"code","message"}} -> ApiError. */
const api=(()=>{
  const transport=CONFIG.transport==="http"?httpTransport:mockTransport;
  let token=null;
  const call=(m,p,b,h)=>transport.request(m,p,b,token,h);
  const n=v=>Number(v)||0,nul=v=>v==null?null:Number(v);
  const list=v=>Array.isArray(v)?v:String(v||"").split(";").map(s=>s.trim()).filter(Boolean);  // Transaction.reasons is ';'-joined
  const txn=t=>t&&{...t,reasons:list(t.reasons)};
  const analysis=a=>{a=a||{};return{
    hasHistoricalAverage:!!a.hasHistoricalAverage,previousCompletedCount:n(a.previousCompletedCount),historicalAverage:n(a.historicalAverage),
    spendingAnomaly:!!a.spendingAnomaly,recentTransactionCount:n(a.recentTransactionCount),rapidFrequency:!!a.rapidFrequency,
    hasPreviousLocation:!!a.hasPreviousLocation,distanceKm:n(a.distanceKm),hoursElapsed:n(a.hoursElapsed),speedKmh:n(a.speedKmh),
    impossibleTravel:!!a.impossibleTravel,hasDeviceHistory:!!a.hasDeviceHistory,newDevice:!!a.newDevice,
    hasMerchantHistory:!!a.hasMerchantHistory,newMerchant:!!a.newMerchant,failedAuthAttempts:n(a.failedAuthAttempts),
    repeatedAuthFailure:!!a.repeatedAuthFailure,wrongOtp:!!a.wrongOtp,score:n(a.score),rawScore:a.rawScore==null?n(a.score):n(a.rawScore),
    status:String(a.status||""),reasons:list(a.reasons)}};
  const outcome=o=>o&&{...o,txn:txn(o.txn),finalAnalysis:analysis(o.finalAnalysis)};
  const pending=r=>({status:String(r.status||""),pendingId:r.pendingId||"",otpRequired:r.otpRequired==null?null:!!r.otpRequired,failedAttempts:nul(r.failedAttempts),
    attemptsRemaining:nul(r.attemptsRemaining),expiresInSeconds:nul(r.expiresInSeconds),
    txn:txn(r.txn)||null,prelim:r.prelim?analysis(r.prelim):null,outcome:outcome(r.outcome)||null});
  const pid=id=>"/transactions/"+encodeURIComponent(id);
  return{
    demoHint:transport.hint||null,
    async login(username,password){const r=await call("POST","/auth/login",{username,password});token=r.token;return r.user},
    logout(){token=null},
    getConfig:()=>call("GET","/config"),
    async getTransactions(){return(await call("GET","/transactions")).map(txn)},
    getCards:()=>call("GET","/cards"),
    addCard:c=>call("POST","/cards",c),
    getLocations:()=>call("GET","/locations"),
    getRoute:(from,to)=>call("GET","/locations/route?from="+encodeURIComponent(from)+"&to="+encodeURIComponent(to)),
    getCardTrail:id=>call("GET","/cards/"+id+"/trail"),
    async startTransaction(req,idempotencyKey){return pending(await call("POST","/transactions/start",req,idempotencyKey?{"Idempotency-Key":idempotencyKey}:undefined))},
    async submitOtp(id,code){return pending(await call("POST",pid(id)+"/otp",{code}))},
    async commitTransaction(id){return pending(await call("POST",pid(id)+"/commit"))},
    async cancelTransaction(id){return pending(await call("POST",pid(id)+"/cancel"))},
    async getReviewQueue(){const q=await call("GET","/review-queue");return{capacity:n(q.capacity),size:n(q.size),items:q.items||[]}},
    popReviewFront:()=>call("POST","/review-queue/front/pop"),
    popReviewRear:()=>call("POST","/review-queue/rear/pop")
  };
})();
