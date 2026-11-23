/* Real REST transport -> backend/server.cpp (C++ REST API). Same interface as the mock transport:
   request(method, path, body, token, headers) -> parsed JSON, or throws ApiError.
   Error contract: non-2xx with body {"error":{"code":"...","message":"...","field":"..."}}.
   Optional 5th argument: extra request headers (e.g. Idempotency-Key). */
class ApiError extends Error{
  constructor(message,{status=0,code="ERROR",field=null}={}){super(message);this.status=status;this.code=code;this.field=field}
}
const httpTransport={
  hint:null,
  async request(method,path,body,token,headers){
    let res;
    try{res=await fetch(CONFIG.apiBaseUrl+path,{method,
      headers:{"Content-Type":"application/json",...(token?{Authorization:"Bearer "+token}:{}),...(headers||{})},
      body:body===undefined?undefined:JSON.stringify(body)})}
    catch(e){throw new ApiError("Cannot reach the server",{code:"NETWORK"})}
    let data=null;try{data=await res.json()}catch(e){}
    if(!res.ok){const er=(data&&data.error)||{};
      throw new ApiError(er.message||"Request failed ("+res.status+")",{status:res.status,code:er.code||"HTTP_"+res.status,field:er.field||null})}
    return data;
  }
};
